#include "renderer/clips/captures/still_pass.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace renderer::clips::captures {
namespace {

// stills.slang StillConstants
struct alignas(16) RegionData {
    int32_t rect[4];
    float map[4];
    int32_t mode[4];
};
struct alignas(16) Constants {
    RegionData region[2];
    int32_t header[4];
    int32_t coding[4];
    uint32_t size[4];
    float target[4];
    float steps[4];
};
static_assert(sizeof(Constants) == 176);

}  // namespace

bool StillPass::Create(Gfx& gfx, uint32_t queued, std::string& error) {
    gfx_ = &gfx;
    auto& NRI = gfx.NRI;
    const nri::DeviceDesc& device_desc = NRI.GetDeviceDesc(*gfx.device);
    stride_ = ((sizeof(Constants) + device_desc.memoryAlignment.constantBufferOffset - 1) /
               device_desc.memoryAlignment.constantBufferOffset) *
              device_desc.memoryAlignment.constantBufferOffset;
    nri::DescriptorRangeDesc textures = {0, 2, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER};
    nri::DescriptorSetDesc sets[] = {{1, &textures, 1}};
    nri::RootDescriptorDesc root[] = {{0, nri::DescriptorType::CONSTANT_BUFFER, nri::StageBits::FRAGMENT_SHADER}};
    nri::PipelineLayoutDesc desc = {};
    desc.rootRegisterSpace = 0;
    desc.rootDescriptors = root;
    desc.rootDescriptorNum = 1;
    desc.descriptorSets = sets;
    desc.descriptorSetNum = 1;
    desc.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
    layout_ = gfx.NewLayout(desc);
    if (!layout_)
        return error = "cannot create a pipeline layout", false;
    constants_ = gfx.NewBuffer(resources_, stride_ * queued, nri::BufferUsageBits::CONSTANT);
    if (!constants_ || !gfx.BindMemory(resources_, 0, 0, nri::MemoryLocation::HOST_UPLOAD))
        return error = "cannot create the constant buffer", false;
    mapped_ = (uint8_t*)NRI.MapBuffer(*constants_, 0, nri::WHOLE_SIZE);
    for (uint32_t q = 0; q < queued; q++)
        constant_views_.push_back(gfx.BufferView(resources_, constants_, nri::BufferView::CONSTANT_BUFFER, q * stride_, stride_));
    // A black texel for an image that is not there
    const size_t first = resources_.textures.size();
    nri::Texture* dummy = gfx.NewTexture(resources_, nri::Format::RGBA16_SFLOAT, 1, 1, nri::TextureUsageBits::SHADER_RESOURCE);
    if (!dummy || !gfx.BindMemory(resources_, resources_.buffers.size(), first, nri::MemoryLocation::DEVICE))
        return error = "cannot create the placeholder texture", false;
    dummy_ = gfx.TextureView(resources_, dummy, nri::TextureView::TEXTURE, nri::Format::RGBA16_SFLOAT);
    const uint16_t texel[4] = {0, 0, 0, 0x3c00};
    nri::TextureSubresourceUploadDesc sub = {};
    sub.slices = texel;
    sub.sliceNum = 1;
    sub.rowPitch = 8;
    sub.slicePitch = 8;
    nri::TextureUploadDesc upload = {};
    upload.subresources = &sub;
    upload.texture = dummy;
    upload.after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE};
    if (NRI.UploadData(*gfx.queue, &upload, 1, nullptr, 0) != nri::Result::SUCCESS)
        return error = "cannot initialise the placeholder texture", false;
    if (!gfx.descriptor_pool)
        return error = "no descriptor pool", false;
    sets_.resize(queued);
    if (NRI.AllocateDescriptorSets(*gfx.descriptor_pool, *layout_, 0, sets_.data(), queued, 0) != nri::Result::SUCCESS)
        return error = "cannot allocate descriptor sets", false;
    return true;
}

void StillPass::Destroy() {
    if (!gfx_ || !gfx_->NRI.HasCore())
        return;
    gfx_->NRI.DeviceWaitIdle(gfx_->device);
    gfx_->Release(resources_);
    sets_.clear();
    constant_views_.clear();
    mapped_ = nullptr;
    gfx_ = nullptr;
}

void StillPass::Record(nri::CommandBuffer& cb, uint32_t queued, nri::Format format, const StillParams& p, nri::Descriptor* a,
                       nri::Descriptor* b) {
    auto& NRI = gfx_->NRI;
    Constants c = {};
    c.header[0] = p.regions.count;
    c.header[1] = p.regions.split;
    c.header[2] = (int32_t)p.target_encoding;
    c.header[3] = p.view == StillView::DIFF ? 1 : 0;
    c.coding[0] = (int32_t)p.coding[0];
    c.coding[1] = (int32_t)p.coding[1];
    c.size[0] = p.size[0][0];
    c.size[1] = p.size[0][1];
    c.size[2] = p.size[1][0];
    c.size[3] = p.size[1][1];
    c.target[0] = p.paper_white;
    c.target[1] = p.hw_srgb ? 1.0f : 0.0f;
    c.target[2] = p.gap_gray;
    c.steps[0] = p.steps[0];
    c.steps[1] = p.steps[1];
    c.steps[2] = 1023.0f;  // a float source's code unit in the difference
    for (int i = 0; i < p.regions.count && i < 2; i++) {
        const Region& r = p.regions.region[i];
        RegionData& d = c.region[i];
        for (int k = 0; k < 4; k++)
            d.rect[k] = r.rect[k];
        d.map[0] = r.origin[0];
        d.map[1] = r.origin[1];
        d.map[2] = r.scale;
        d.mode[0] = r.version;
        d.mode[1] = p.box_below_one && r.scale < 1.0f - 1e-6f ? 1 : 0;
        d.mode[2] = (p.exists[0] ? 1 : 0) | (p.exists[1] ? 2 : 0);
    }
    std::memcpy(mapped_ + queued * stride_, &c, sizeof(c));
    nri::Descriptor* views[2] = {a ? a : (b ? b : dummy_), b ? b : (a ? a : dummy_)};
    const nri::UpdateDescriptorRangeDesc update = {sets_[queued], 0, 0, views, 2};
    NRI.UpdateDescriptorRanges(&update, 1);
    NRI.CmdSetPipelineLayout(cb, nri::BindPoint::GRAPHICS, *layout_);
    NRI.CmdSetPipeline(cb, *gfx_->Fullscreen(layout_, "still_fs", format));
    NRI.CmdSetRootDescriptor(cb, {0, constant_views_[queued], 0, nri::BindPoint::GRAPHICS});
    NRI.CmdSetDescriptorSet(cb, {0, sets_[queued], nri::BindPoint::GRAPHICS});
    NRI.CmdDraw(cb, {3, 1, 0, 0});
}

}  // namespace renderer::clips::captures
