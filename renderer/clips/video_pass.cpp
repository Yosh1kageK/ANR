#include "renderer/clips/video_pass.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace renderer::clips {

bool VideoPass::Create(Gfx& gfx, uint32_t queued_frames, std::string& error) {
    gfx_ = &gfx;
    queued_ = queued_frames;
    auto& NRI = gfx.NRI;
    const nri::DeviceDesc& device_desc = NRI.GetDeviceDesc(*gfx.device);
    stride_ = ((sizeof(VideoConstants) + device_desc.memoryAlignment.constantBufferOffset - 1) /
               device_desc.memoryAlignment.constantBufferOffset) *
              device_desc.memoryAlignment.constantBufferOffset;

    // Layouts: the constants as a root descriptor, the pictures' textures in a set.
    for (int raw = 0; raw < 2; raw++) {
        nri::DescriptorRangeDesc textures = {0, raw ? 2u : 4u, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER};
        nri::DescriptorSetDesc sets[] = {{1, &textures, 1}};
        nri::RootDescriptorDesc root[] = {{0, nri::DescriptorType::CONSTANT_BUFFER, nri::StageBits::FRAGMENT_SHADER}};
        nri::PipelineLayoutDesc desc = {};
        desc.rootRegisterSpace = 0;
        desc.rootDescriptors = root;
        desc.rootDescriptorNum = 1;
        desc.descriptorSets = sets;
        desc.descriptorSetNum = 1;
        desc.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        (raw ? raw_layout_ : video_layout_) = gfx.NewLayout(desc);
        if (!(raw ? raw_layout_ : video_layout_)) {
            error = "cannot create a pipeline layout";
            return false;
        }
    }

    // Constants: a slot per queued frame, written through a persistent mapping.
    constants_ = gfx.NewBuffer(resources_, stride_ * queued_frames, nri::BufferUsageBits::CONSTANT);
    if (!constants_ || !gfx.BindMemory(resources_, 0, 0, nri::MemoryLocation::HOST_UPLOAD)) {
        error = "cannot create the constant buffer";
        return false;
    }
    mapped_ = (uint8_t*)NRI.MapBuffer(*constants_, 0, nri::WHOLE_SIZE);
    for (uint32_t q = 0; q < queued_frames; q++)
        constant_views_.push_back(
            gfx.BufferView(resources_, constants_, nri::BufferView::CONSTANT_BUFFER, q * stride_, stride_));

    // Dummy pictures for a recording that has none this frame: one black texel each.
    const size_t first_texture = resources_.textures.size();
    nri::Texture* luma = gfx.NewTexture(resources_, nri::Format::R8_UNORM, 1, 1, nri::TextureUsageBits::SHADER_RESOURCE);
    nri::Texture* chroma = gfx.NewTexture(resources_, nri::Format::RG8_UNORM, 1, 1, nri::TextureUsageBits::SHADER_RESOURCE);
    nri::Texture* picture = gfx.NewTexture(resources_, nri::Format::RGBA8_UNORM, 1, 1, nri::TextureUsageBits::SHADER_RESOURCE);
    if (!luma || !chroma || !picture || !gfx.BindMemory(resources_, resources_.buffers.size(), first_texture, nri::MemoryLocation::DEVICE)) {
        error = "cannot create the placeholder textures";
        return false;
    }
    dummy_luma_ = gfx.TextureView(resources_, luma, nri::TextureView::TEXTURE, nri::Format::R8_UNORM);
    dummy_chroma_ = gfx.TextureView(resources_, chroma, nri::TextureView::TEXTURE, nri::Format::RG8_UNORM);
    dummy_picture_ = gfx.TextureView(resources_, picture, nri::TextureView::TEXTURE, nri::Format::RGBA8_UNORM);
    {
        const uint8_t texel[4] = {0, 0, 0, 255};
        nri::TextureSubresourceUploadDesc sub[3] = {};
        nri::TextureUploadDesc uploads[3] = {};
        nri::Texture* textures[3] = {luma, chroma, picture};
        for (int i = 0; i < 3; i++) {
            sub[i].slices = texel;
            sub[i].sliceNum = 1;
            sub[i].rowPitch = 4;
            sub[i].slicePitch = 4;
            uploads[i].subresources = &sub[i];
            uploads[i].texture = textures[i];
            uploads[i].after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE};
        }
        if (NRI.UploadData(*gfx.queue, uploads, 3, nullptr, 0) != nri::Result::SUCCESS) {
            error = "cannot initialise the placeholder textures";
            return false;
        }
    }

    // Descriptor sets: one per queued frame for each layout, from the viewer's pool.
    if (!gfx.descriptor_pool) {
        error = "no descriptor pool";
        return false;
    }
    video_sets_.resize(queued_frames);
    raw_sets_.resize(queued_frames);
    if (NRI.AllocateDescriptorSets(*gfx.descriptor_pool, *video_layout_, 0, video_sets_.data(), queued_frames, 0) != nri::Result::SUCCESS ||
        NRI.AllocateDescriptorSets(*gfx.descriptor_pool, *raw_layout_, 0, raw_sets_.data(), queued_frames, 0) != nri::Result::SUCCESS) {
        error = "cannot allocate descriptor sets";
        return false;
    }
    return true;
}

void VideoPass::Destroy() {
    if (!gfx_ || !gfx_->NRI.HasCore())
        return;
    gfx_->NRI.DeviceWaitIdle(gfx_->device);
    gfx_->Release(resources_);
    video_sets_.clear();
    raw_sets_.clear();
    constant_views_.clear();
    mapped_ = nullptr;
    gfx_ = nullptr;
}

void VideoPass::WriteConstants(uint32_t queued, const VideoConstants& constants) {
    std::memcpy(mapped_ + queued * stride_, &constants, sizeof(constants));
}

void VideoPass::RecordVideo(nri::CommandBuffer& cb, uint32_t queued, nri::Format format, const VideoConstants& constants,
                            nri::Descriptor* const luma[2], nri::Descriptor* const chroma[2]) {
    auto& NRI = gfx_->NRI;
    WriteConstants(queued, constants);
    // A recording without a picture reads the other's (or the placeholder): the shader never uses it.
    nri::Descriptor* views[4] = {luma[0] ? luma[0] : (luma[1] ? luma[1] : dummy_luma_),
                                 chroma[0] ? chroma[0] : (chroma[1] ? chroma[1] : dummy_chroma_),
                                 luma[1] ? luma[1] : (luma[0] ? luma[0] : dummy_luma_),
                                 chroma[1] ? chroma[1] : (chroma[0] ? chroma[0] : dummy_chroma_)};
    const nri::UpdateDescriptorRangeDesc update = {video_sets_[queued], 0, 0, views, 4};
    NRI.UpdateDescriptorRanges(&update, 1);
    NRI.CmdSetPipelineLayout(cb, nri::BindPoint::GRAPHICS, *video_layout_);
    NRI.CmdSetPipeline(cb, *gfx_->Fullscreen(video_layout_, "video_fs", format));
    NRI.CmdSetRootDescriptor(cb, {0, constant_views_[queued], 0, nri::BindPoint::GRAPHICS});
    NRI.CmdSetDescriptorSet(cb, {0, video_sets_[queued], nri::BindPoint::GRAPHICS});
    NRI.CmdDraw(cb, {3, 1, 0, 0});
}

void VideoPass::RecordRaw(nri::CommandBuffer& cb, uint32_t queued, nri::Format format, const VideoConstants& constants,
                          nri::Descriptor* const picture[2]) {
    auto& NRI = gfx_->NRI;
    WriteConstants(queued, constants);
    nri::Descriptor* views[2] = {picture[0] ? picture[0] : (picture[1] ? picture[1] : dummy_picture_),
                                 picture[1] ? picture[1] : (picture[0] ? picture[0] : dummy_picture_)};
    const nri::UpdateDescriptorRangeDesc update = {raw_sets_[queued], 0, 0, views, 2};
    NRI.UpdateDescriptorRanges(&update, 1);
    NRI.CmdSetPipelineLayout(cb, nri::BindPoint::GRAPHICS, *raw_layout_);
    NRI.CmdSetPipeline(cb, *gfx_->Fullscreen(raw_layout_, "raw_fs", format));
    NRI.CmdSetRootDescriptor(cb, {0, constant_views_[queued], 0, nri::BindPoint::GRAPHICS});
    NRI.CmdSetDescriptorSet(cb, {0, raw_sets_[queued], nri::BindPoint::GRAPHICS});
    NRI.CmdDraw(cb, {3, 1, 0, 0});
}

}  // namespace renderer::clips
