#include "renderer/clips/composer.h"

#include <algorithm>

#include "renderer/display.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace {

struct UiConstants {  // clips.slang ui_layer_fs
    uint32_t encoding;
    float paper_white;
    float pad[2];
};

nri::TextureBarrierDesc Barrier(nri::Texture* texture, nri::AccessLayoutStage before, nri::AccessLayoutStage after) {
    nri::TextureBarrierDesc b = {};
    b.texture = texture;
    b.before = before;
    b.after = after;
    b.layerNum = 1;
    b.mipNum = 1;
    return b;
}

}  // namespace

bool OffscreenComposer::Create(Gfx& gfx, uint32_t width, uint32_t height, nri::Format format, uint32_t slots, std::string& error,
                               float paper_white) {
    gfx_ = &gfx;
    auto& NRI = gfx.NRI;
    width_ = width;
    height_ = height;
    format_ = format;
    paper_white_ = paper_white;
    const bool hdr = format == nri::Format::RGBA16_UNORM;
    bytes_per_pixel_ = hdr ? 8 : 4;
    target_ = gfx.NewTexture(resources_, format, width, height, nri::TextureUsageBits::COLOR_ATTACHMENT);
    if (!target_ || !gfx.BindMemory(resources_, 0, 0, nri::MemoryLocation::DEVICE)) {
        error = "cannot create the offscreen target";
        return false;
    }
    target_view_ = gfx.TextureView(resources_, target_, nri::TextureView::COLOR_ATTACHMENT, format);
    const nri::DeviceDesc& device_desc = NRI.GetDeviceDesc(*gfx.device);
    const uint64_t alignment = std::max<uint32_t>(device_desc.memoryAlignment.uploadBufferTextureRow, 1u);
    row_pitch_ = (uint32_t)(((uint64_t)width * bytes_per_pixel_ + alignment - 1) / alignment * alignment);
    slots_.resize(slots);
    for (Slot& s : slots_) {
        nri::BufferDesc desc = {};
        desc.size = (uint64_t)row_pitch_ * height;
        nri::ResourceGroupDesc group = {};
        group.memoryLocation = nri::MemoryLocation::HOST_READBACK;
        if (NRI.CreateBuffer(*gfx.device, desc, s.buffer) != nri::Result::SUCCESS)
            return error = "cannot create a readback buffer", false;
        group.buffers = &s.buffer;
        group.bufferNum = 1;
        if (NRI.AllocateAndBindMemory(*gfx.device, group, &s.memory) != nri::Result::SUCCESS)
            return error = "cannot allocate a readback buffer", false;
        s.data = (uint8_t*)NRI.MapBuffer(*s.buffer, 0, nri::WHOLE_SIZE);
        if (!s.data || NRI.CreateCommandAllocator(*gfx.queue, s.allocator) != nri::Result::SUCCESS ||
            NRI.CreateCommandBuffer(*s.allocator, s.command_buffer) != nri::Result::SUCCESS)
            return error = "cannot create a command buffer", false;
    }
    if (NRI.CreateFence(*gfx.device, nri::FenceDesc{}, fence_) != nri::Result::SUCCESS)
        return error = "cannot create a fence", false;
    if (!gfx.descriptor_pool && !gfx.CreateDescriptorPool(16, 32))
        return error = "cannot create a descriptor pool", false;

    // The overlay's draw path: its own streamer and NRI ImGui instance
    nri::StreamerDesc streamer_desc = {};
    streamer_desc.dynamicBufferMemoryLocation = nri::MemoryLocation::HOST_UPLOAD;
    streamer_desc.dynamicBufferDesc = {0, 0, nri::BufferUsageBits::VERTEX | nri::BufferUsageBits::INDEX};
    streamer_desc.constantBufferMemoryLocation = nri::MemoryLocation::HOST_UPLOAD;
    streamer_desc.queuedFrameNum = slots;
    streamer_desc.hostDataCapacity = IMGUI_HOST_DATA_CAPACITY;
    if (NRI.CreateStreamer(*gfx.device, streamer_desc, streamer_) != nri::Result::SUCCESS)
        return error = "cannot create the streamer", false;
    if (nri::nriGetInterface(*gfx.device, NRI_INTERFACE(nri::ImguiInterface), &imgui_interface_) != nri::Result::SUCCESS)
        return error = "NRI has no ImGui interface", false;
    nri::ImguiDesc imgui_desc = {};
    imgui_desc.descriptorPoolSize = 16;
    if (imgui_interface_.CreateImgui(*gfx.device, imgui_desc, imgui_) != nri::Result::SUCCESS)
        return error = "cannot create the ImGui renderer", false;
    list_ = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());

    if (hdr) {  // the overlay's layer and the pass that blends it (clips.slang ui_layer_fs)
        nri::DescriptorRangeDesc texture = {0, 1, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER};
        nri::DescriptorSetDesc sets[] = {{1, &texture, 1}};
        nri::RootConstantDesc constants[] = {{0, sizeof(UiConstants), nri::StageBits::FRAGMENT_SHADER}};
        nri::PipelineLayoutDesc desc = {};
        desc.rootConstants = constants;
        desc.rootConstantNum = 1;
        desc.descriptorSets = sets;
        desc.descriptorSetNum = 1;
        desc.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        layer_layout_ = gfx.NewLayout(desc);
        if (!layer_layout_ || NRI.AllocateDescriptorSets(*gfx.descriptor_pool, *layer_layout_, 0, &layer_set_, 1, 0) != nri::Result::SUCCESS)
            return error = "cannot create the overlay layer's descriptors", false;
        layer_ = gfx.NewTexture(layer_resources_, nri::Format::RGBA16_SFLOAT, width, height,
                                nri::TextureUsageBits::COLOR_ATTACHMENT | nri::TextureUsageBits::SHADER_RESOURCE);
        if (!layer_ || !gfx.BindMemory(layer_resources_, 0, 0, nri::MemoryLocation::DEVICE))
            return error = "cannot create the overlay layer", false;
        layer_target_ = gfx.TextureView(layer_resources_, layer_, nri::TextureView::COLOR_ATTACHMENT, nri::Format::RGBA16_SFLOAT);
        layer_view_ = gfx.TextureView(layer_resources_, layer_, nri::TextureView::TEXTURE, nri::Format::RGBA16_SFLOAT);
        const nri::UpdateDescriptorRangeDesc update = {layer_set_, 0, 0, &layer_view_, 1};
        NRI.UpdateDescriptorRanges(&update, 1);
    }
    return true;
}

void OffscreenComposer::Destroy() {
    if (!gfx_ || !gfx_->NRI.HasCore() || !gfx_->device) {
        gfx_ = nullptr;
        return;
    }
    auto& NRI = gfx_->NRI;
    NRI.DeviceWaitIdle(gfx_->device);
    for (Slot& s : slots_) {
        if (s.command_buffer)
            NRI.DestroyCommandBuffer(s.command_buffer);
        if (s.allocator)
            NRI.DestroyCommandAllocator(s.allocator);
        if (s.buffer)
            NRI.DestroyBuffer(s.buffer);
        if (s.memory)
            NRI.FreeMemory(s.memory);
    }
    slots_.clear();
    if (fence_)
        NRI.DestroyFence(fence_);
    fence_ = nullptr;
    if (imgui_)
        imgui_interface_.DestroyImgui(imgui_);
    imgui_ = nullptr;
    if (streamer_)
        NRI.DestroyStreamer(streamer_);
    streamer_ = nullptr;
    gfx_->Release(resources_);
    gfx_->Release(layer_resources_);
    list_.reset();
    gfx_ = nullptr;
}

ImDrawList* OffscreenComposer::BeginOverlay() {
    list_->_ResetForNewFrame();
    list_->PushTexture(ImGui::GetIO().Fonts->TexRef);
    list_->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2((float)width_, (float)height_), false);
    return list_.get();
}

void OffscreenComposer::Wait(uint32_t slot) {
    gfx_->NRI.Wait(*fence_, slots_[slot].fence_value);
}

void OffscreenComposer::Record(uint32_t index, const std::function<void(nri::CommandBuffer&)>& pictures) {
    auto& NRI = gfx_->NRI;
    Slot& slot = slots_[index];
    NRI.ResetCommandAllocator(*slot.allocator);
    nri::CommandBuffer& cb = *slot.command_buffer;
    NRI.BeginCommandBuffer(cb, gfx_->descriptor_pool);
    const bool hdr = format_ == nri::Format::RGBA16_UNORM;
    const nri::AccessLayoutStage none = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL};
    const nri::AccessLayoutStage attachment = {nri::AccessBits::COLOR_ATTACHMENT_WRITE, nri::Layout::COLOR_ATTACHMENT,
                                               nri::StageBits::COLOR_ATTACHMENT};
    nri::TextureBarrierDesc target = Barrier(target_, none, attachment);
    nri::BarrierDesc barrier = {};
    barrier.textures = &target;
    barrier.textureNum = 1;
    NRI.CmdBarrier(cb, barrier);

    // The overlay's draw data
    const ImDrawList* lists[1] = {list_.get()};
    nri::CopyImguiDataDesc copy = {};
    copy.drawLists = lists;
    copy.drawListNum = 1;
    copy.textures = ImGui::GetPlatformIO().Textures.Data;
    copy.textureNum = (uint32_t)ImGui::GetPlatformIO().Textures.Size;
    nri::ImguiRenderData draw_data = {};
    imgui_interface_.CmdCopyImguiData(cb, *streamer_, *imgui_, copy, draw_data);

    nri::AttachmentDesc color = {};
    color.descriptor = target_view_;
    color.loadOp = nri::LoadOp::CLEAR;
    nri::RenderingDesc rendering = {};
    rendering.colors = &color;
    rendering.colorNum = 1;
    const nri::Viewport viewport = {0.0f, 0.0f, (float)width_, (float)height_, 0.0f, 1.0f};
    const nri::Rect scissor = {0, 0, (nri::Dim_t)width_, (nri::Dim_t)height_};
    NRI.CmdBeginRendering(cb, rendering);
    NRI.CmdSetViewports(cb, &viewport, 1);
    NRI.CmdSetScissors(cb, &scissor, 1);
    pictures(cb);
    nri::DrawImguiDesc draw = {};
    draw.displaySize = {(nri::Dim_t)width_, (nri::Dim_t)height_};
    draw.hdrScale = 1.0f;
    if (!hdr) {
        draw.attachmentFormat = format_;
        draw.linearColor = false;
        imgui_interface_.CmdDrawImgui(cb, draw_data, draw);
    }
    NRI.CmdEndRendering(cb);
    if (hdr) {  // the layer (transparent, linear), then its blend into the PQ codes by coverage
        nri::TextureBarrierDesc layer = Barrier(layer_, none, attachment);
        nri::BarrierDesc b = {};
        b.textures = &layer;
        b.textureNum = 1;
        NRI.CmdBarrier(cb, b);
        nri::AttachmentDesc layer_color = {};
        layer_color.descriptor = layer_target_;
        layer_color.loadOp = nri::LoadOp::CLEAR;
        layer_color.storeOp = nri::StoreOp::STORE;
        nri::RenderingDesc layer_rendering = {};
        layer_rendering.colors = &layer_color;
        layer_rendering.colorNum = 1;
        NRI.CmdBeginRendering(cb, layer_rendering);
        NRI.CmdSetViewports(cb, &viewport, 1);
        NRI.CmdSetScissors(cb, &scissor, 1);
        draw.attachmentFormat = nri::Format::RGBA16_SFLOAT;
        draw.linearColor = false;
        imgui_interface_.CmdDrawImgui(cb, draw_data, draw);
        NRI.CmdEndRendering(cb);
        layer.before = layer.after;
        layer.after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER};
        NRI.CmdBarrier(cb, b);
        color.loadOp = nri::LoadOp::LOAD;
        NRI.CmdBeginRendering(cb, rendering);
        NRI.CmdSetViewports(cb, &viewport, 1);
        NRI.CmdSetScissors(cb, &scissor, 1);
        const UiConstants constants = {PQ10, paper_white_, {0.0f, 0.0f}};
        NRI.CmdSetPipelineLayout(cb, nri::BindPoint::GRAPHICS, *layer_layout_);
        NRI.CmdSetPipeline(cb, *gfx_->Fullscreen(layer_layout_, "ui_layer_fs", format_, true));
        NRI.CmdSetRootConstants(cb, {0, &constants, sizeof(constants), 0, nri::BindPoint::GRAPHICS});
        NRI.CmdSetDescriptorSet(cb, {0, layer_set_, nri::BindPoint::GRAPHICS});
        NRI.CmdDraw(cb, {3, 1, 0, 0});
        NRI.CmdEndRendering(cb);
    }

    target.before = target.after;
    target.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
    NRI.CmdBarrier(cb, barrier);
    nri::TextureDataLayoutDesc layout = {0, row_pitch_, row_pitch_ * height_};
    nri::TextureRegionDesc region = {};
    region.width = (nri::Dim_t)width_;
    region.height = (nri::Dim_t)height_;
    region.depth = 1;
    NRI.CmdReadbackTextureToBuffer(cb, *slot.buffer, layout, *target_, region);
    NRI.EndCommandBuffer(cb);

    const uint64_t value = ++submitted_;
    nri::FenceSubmitDesc signal = {fence_, value, nri::StageBits::ALL};
    nri::CommandBuffer* buffers[] = {&cb};
    nri::QueueSubmitDesc submit = {};
    submit.commandBuffers = buffers;
    submit.commandBufferNum = 1;
    submit.signalFences = &signal;
    submit.signalFenceNum = 1;
    NRI.QueueSubmit(*gfx_->queue, submit);
    NRI.EndStreamerFrame(*streamer_);
    slot.fence_value = value;
}

}  // namespace renderer::clips
