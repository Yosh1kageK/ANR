#include "renderer/clips/swap_output.h"

#include <algorithm>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace renderer::clips {
namespace {

struct UiConstants {
    uint32_t encoding;
    float paper_white;
    float pad[2];
};

}  // namespace

void SwapOutput::QueryDisplay(Gfx& gfx, App& app, SDL_Window* window, const SwapOptions& options) {
    gfx_ = &gfx;
    app_ = &app;
    vsync = options.vsync;
    min_textures = options.min_textures;
    hdr_metadata = options.hdr_metadata;
    wanted_ = options.wanted;
    hdr = HdrState();
    hdr.paper_white_set = options.paper_white_set;
    hdr.peak_set = options.peak_set;
    if (hdr.paper_white_set)
        hdr.paper_white = options.paper_white;
    if (hdr.peak_set)
        hdr.peak = options.peak;
#ifdef __APPLE__
    QueryHdrSurface(app.GetWindow(), nullptr, nullptr, nullptr, supported_[PQ10], supported_[SCRGB16]);
#else
    QueryHdrSurface(app.GetWindow(), gfx.vk.instance, gfx.vk.physical_device, gfx.vk.get_instance_proc,
                    supported_[PQ10], supported_[SCRGB16]);
#endif
    ReadEdidPeak(window, hdr);
    spdlog::info("display: HDR10 {}, scRGB {}{}", supported_[PQ10] ? "offered" : "not offered",
                 supported_[SCRGB16] ? "offered" : "not offered",
                 hdr.reported_peak > 0.0f ? ", peak " + std::to_string((int)hdr.reported_peak) + " nits from the " + hdr.peak_source
                                          : std::string());
    UpdateDefaults(window);
}

void SwapOutput::RequeryFormats() {
    supported_[PQ10] = supported_[SCRGB16] = false;
#ifdef __APPLE__
    QueryHdrSurface(app_->GetWindow(), nullptr, nullptr, nullptr, supported_[PQ10], supported_[SCRGB16]);
#else
    QueryHdrSurface(app_->GetWindow(), gfx_->vk.instance, gfx_->vk.physical_device, gfx_->vk.get_instance_proc,
                    supported_[PQ10], supported_[SCRGB16]);
#endif
}

void SwapOutput::UpdateDefaults(SDL_Window* window) {
    UpdateHdrDefaults(window, hdr, wanted_ == PQ10);
}

bool SwapOutput::Create(Gfx& gfx, App& app, uint32_t queued_frames, std::string& error) {
    gfx_ = &gfx;
    app_ = &app;
    queued_ = queued_frames;
    auto& NRI = gfx.NRI;
    const nri::SwapChainFormat formats[ENCODINGS] = {nri::SwapChainFormat::BT709_G22_8BIT,
                                                     nri::SwapChainFormat::BT2020_G2084_10BIT,
                                                     nri::SwapChainFormat::BT709_G10_16BIT};
    if (!supported_[wanted_]) {
        spdlog::warn("the surface offers no {} swap chain; SDR", SWAP_LABELS[wanted_]);
        wanted_ = SRGB8;
    }
    size_ = app.GetOutputResolution();
    nri::SwapChainDesc swap = {};
    swap.window = app.GetWindow();
    swap.queue = gfx.queue;
    swap.format = formats[wanted_];
    // Without vsync: tearing (immediate) on X11 and Windows; on Wayland, which has no immediate mode here, mailbox (NRI
    // falls back to FIFO, which is vsync, when it finds no immediate mode); on macOS, Metal's no-vsync mode.
    swap.flags = vsync ? nri::SwapChainBits::VSYNC
                       : (METAL || app.IsWayland() ? nri::SwapChainBits::NONE : nri::SwapChainBits::ALLOW_TEARING);
    swap.width = (nri::Dim_t)size_.x;
    swap.height = (nri::Dim_t)size_.y;
    swap.textureNum = (uint8_t)std::max<uint32_t>(app.GetOptimalSwapChainTextureNum(), min_textures);
    swap.queuedFrameNum = app.GetQueuedFrameNum();
    if (wanted_ != SRGB8 && hdr_metadata) {  // the content's peak; frame averages stay below the paper white
        swap.hdrMaxLuminance = hdr.peak;
        swap.hdrMaxFrameAverageLuminance = hdr.paper_white;
        if (metadata) {  // the content's own mastering display
            swap.hdrMaxLuminance = metadata->max_luminance;
            swap.hdrMinLuminance = metadata->min_luminance;
            swap.hdrMaxContentLightLevel = metadata->max_cll;
            swap.hdrMaxFrameAverageLuminance = metadata->max_fall;
            swap.hdrRec2020Primaries = metadata->rec2020;
        }
    }
    metadata_sent_ = false;
    nri::Result result = NRI.CreateSwapChain(*gfx.device, swap, swap_chain_);
    if (result != nri::Result::SUCCESS && wanted_ != SRGB8) {
        spdlog::error("{} swap chain creation failed (nri::Result {}); SDR", SWAP_LABELS[wanted_], (int)result);
        supported_[wanted_] = false;
        wanted_ = SRGB8;
        swap.format = formats[SRGB8];
        swap.hdrMaxLuminance = swap.hdrMaxFrameAverageLuminance = 0.0f;
        swap.hdrMinLuminance = swap.hdrMaxContentLightLevel = 0.0f;
        swap.hdrRec2020Primaries = false;
        result = NRI.CreateSwapChain(*gfx.device, swap, swap_chain_);
    }
    if (result != nri::Result::SUCCESS) {
        error = "the swap chain could not be created (nri::Result " + std::to_string((int)result) + ")";
        return false;
    }
    uint32_t count = 0;
    nri::Texture* const* textures = NRI.GetSwapChainTextures(*swap_chain_, count);
    format_ = NRI.GetTextureDesc(*textures[0]).format;
    encoding_ = wanted_;
    metadata_sent_ = encoding_ != SRGB8 && swap.hdrMaxLuminance > 0.0f;
    encode_srgb_ = !(format_ == nri::Format::BGRA8_SRGB || format_ == nri::Format::RGBA8_SRGB);
    spdlog::info("swap chain: {}, format {} (nri::Format), VkColorSpaceKHR {}, {}x{}{}", SWAP_LABELS[encoding_],
                 (int)format_, SWAP_COLOR_SPACES[encoding_], size_.x, size_.y,
                 swap.hdrMaxLuminance > 0.0f ? ", HDR metadata " + std::to_string((int)hdr.peak) + " nits" : "");
    for (uint32_t i = 0; i < count; i++) {
        SwapChainTexture& s = textures_.emplace_back();
        s = {};
        nri::TextureViewDesc view = {textures[i], nri::TextureView::COLOR_ATTACHMENT, format_};
        NRI.CreateTextureView(view, s.colorAttachment);
        NRI.CreateFence(*gfx.device, {nri::SWAPCHAIN_SEMAPHORE}, s.acquireSemaphore);
        NRI.CreateFence(*gfx.device, {nri::SWAPCHAIN_SEMAPHORE}, s.releaseSemaphore);
        s.texture = textures[i];
        s.attachmentFormat = format_;
    }
    if (encoding_ == PQ10) {
        // The UI of an HDR10 swap chain: a linear layer at the paper white, composited by coverage (clips.slang
        // ui_layer_fs).
        if (!ui_layout_) {
            nri::DescriptorRangeDesc texture = {0, 1, nri::DescriptorType::TEXTURE, nri::StageBits::FRAGMENT_SHADER};
            nri::DescriptorSetDesc sets[] = {{1, &texture, 1}};
            nri::RootConstantDesc constants[] = {{0, sizeof(UiConstants), nri::StageBits::FRAGMENT_SHADER}};
            nri::PipelineLayoutDesc desc = {};
            desc.rootConstants = constants;
            desc.rootConstantNum = 1;
            desc.descriptorSets = sets;
            desc.descriptorSetNum = 1;
            desc.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
            ui_layout_ = gfx.NewLayout(desc);
            if (ui_layout_ && gfx.descriptor_pool)
                NRI.AllocateDescriptorSets(*gfx.descriptor_pool, *ui_layout_, 0, &ui_set_, 1, 0);
        }
        ui_layer_ = gfx.NewTexture(ui_resources_, nri::Format::RGBA16_SFLOAT, size_.x, size_.y,
                                   nri::TextureUsageBits::COLOR_ATTACHMENT | nri::TextureUsageBits::SHADER_RESOURCE);
        if (!ui_layer_ || !gfx.BindMemory(ui_resources_, 0, 0, nri::MemoryLocation::DEVICE) || !ui_set_) {
            error = "cannot create the HDR10 UI layer";
            return false;
        }
        ui_target_ = gfx.TextureView(ui_resources_, ui_layer_, nri::TextureView::COLOR_ATTACHMENT, nri::Format::RGBA16_SFLOAT);
        ui_view_ = gfx.TextureView(ui_resources_, ui_layer_, nri::TextureView::TEXTURE, nri::Format::RGBA16_SFLOAT);
        const nri::UpdateDescriptorRangeDesc update = {ui_set_, 0, 0, &ui_view_, 1};
        NRI.UpdateDescriptorRanges(&update, 1);
    }
    dirty = false;
    return true;
}

void SwapOutput::Destroy() {
    if (!gfx_ || !gfx_->NRI.HasCore())
        return;
    auto& NRI = gfx_->NRI;
    for (SwapChainTexture& s : textures_) {
        NRI.DestroyFence(s.acquireSemaphore);
        NRI.DestroyFence(s.releaseSemaphore);
        NRI.DestroyDescriptor(s.colorAttachment);
    }
    textures_.clear();
    if (swap_chain_)
        NRI.DestroySwapChain(swap_chain_);
    swap_chain_ = nullptr;
    gfx_->Release(ui_resources_);
    ui_layer_ = nullptr;
}

bool SwapOutput::Acquire(uint32_t frame_index, Frame& out) {
    const uint32_t semaphore = frame_index % (uint32_t)textures_.size();
    nri::Fence* acquired = textures_[semaphore].acquireSemaphore;
    uint32_t index = 0;
    if (gfx_->NRI.AcquireNextTexture(*swap_chain_, *acquired, index) != nri::Result::SUCCESS) {
        dirty = true;
        return false;
    }
    out.swap = &textures_[index];
    out.index = semaphore;
    return true;
}

void SwapOutput::Record(nri::CommandBuffer& cb, nri::Streamer& streamer, const Frame& frame,
                        const std::function<void(nri::Format format)>& video, bool ui, nri::Buffer* readback,
                        uint32_t row_pitch) {
    auto& NRI = gfx_->NRI;
    const SwapChainTexture& swap = *frame.swap;
    nri::TextureBarrierDesc to_target = {};
    to_target.texture = swap.texture;
    to_target.after = {nri::AccessBits::COLOR_ATTACHMENT_WRITE, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT};
    to_target.layerNum = 1;
    to_target.mipNum = 1;
    nri::BarrierDesc barrier = {};
    barrier.textures = &to_target;
    barrier.textureNum = 1;
    NRI.CmdBarrier(cb, barrier);
    const bool draw_ui = ui && app_->HasUserInterface() && ImGui::GetDrawData();
    const nri::ImguiRenderData imgui = draw_ui ? app_->CmdCopyImguiData(cb, streamer) : nri::ImguiRenderData{};
    nri::AttachmentDesc color = {};
    color.descriptor = swap.colorAttachment;
    color.loadOp = nri::LoadOp::CLEAR;
    nri::RenderingDesc rendering = {};
    rendering.colors = &color;
    rendering.colorNum = 1;
    const uint32_t w = size_.x, h = size_.y;
    const nri::Viewport viewport = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    const nri::Rect scissor = {0, 0, (nri::Dim_t)w, (nri::Dim_t)h};
    NRI.CmdBeginRendering(cb, rendering);
    NRI.CmdSetViewports(cb, &viewport, 1);
    NRI.CmdSetScissors(cb, &scissor, 1);
    video(format_);
    // The UI at the paper white: SDR as it was; scRGB in linear values; HDR10 through its own layer (below)
    if (draw_ui && encoding_ == SRGB8)
        app_->CmdDrawImgui(cb, imgui, swap.attachmentFormat, 1.0f, !encode_srgb_);
    else if (draw_ui && encoding_ == SCRGB16)
        app_->CmdDrawImgui(cb, imgui, swap.attachmentFormat, hdr.paper_white / SCRGB_NITS, false);
    NRI.CmdEndRendering(cb);
    if (draw_ui && encoding_ == PQ10 && ui_layer_ && ImGui::GetDrawData()->TotalVtxCount > 0) {
        nri::TextureBarrierDesc layer = {};
        layer.texture = ui_layer_;
        layer.before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL};
        layer.after = {nri::AccessBits::COLOR_ATTACHMENT_WRITE, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT};
        layer.layerNum = 1;
        layer.mipNum = 1;
        nri::BarrierDesc b = {};
        b.textures = &layer;
        b.textureNum = 1;
        NRI.CmdBarrier(cb, b);
        nri::AttachmentDesc ui_color = {};
        ui_color.descriptor = ui_target_;
        ui_color.loadOp = nri::LoadOp::CLEAR;  // transparent
        ui_color.storeOp = nri::StoreOp::STORE;
        nri::RenderingDesc ui_rendering = {};
        ui_rendering.colors = &ui_color;
        ui_rendering.colorNum = 1;
        NRI.CmdBeginRendering(cb, ui_rendering);
        NRI.CmdSetViewports(cb, &viewport, 1);
        NRI.CmdSetScissors(cb, &scissor, 1);
        app_->CmdDrawImgui(cb, imgui, nri::Format::RGBA16_SFLOAT, 1.0f, false);
        NRI.CmdEndRendering(cb);
        layer.before = layer.after;
        layer.after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER};
        NRI.CmdBarrier(cb, b);
        color.loadOp = nri::LoadOp::LOAD;
        NRI.CmdBeginRendering(cb, rendering);
        NRI.CmdSetViewports(cb, &viewport, 1);
        NRI.CmdSetScissors(cb, &scissor, 1);
        const UiConstants constants = {PQ10, hdr.paper_white, {0.0f, 0.0f}};
        NRI.CmdSetPipelineLayout(cb, nri::BindPoint::GRAPHICS, *ui_layout_);
        NRI.CmdSetPipeline(cb, *gfx_->Fullscreen(ui_layout_, "ui_layer_fs", format_, true));
        NRI.CmdSetRootConstants(cb, {0, &constants, sizeof(constants), 0, nri::BindPoint::GRAPHICS});
        NRI.CmdSetDescriptorSet(cb, {0, ui_set_, nri::BindPoint::GRAPHICS});
        NRI.CmdDraw(cb, {3, 1, 0, 0});
        NRI.CmdEndRendering(cb);
    }
    if (readback) {
        to_target.before = to_target.after;
        to_target.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        NRI.CmdBarrier(cb, barrier);
        const nri::TextureDataLayoutDesc layout = {0, row_pitch, row_pitch * h};
        nri::TextureRegionDesc region = {};
        region.width = (nri::Dim_t)w;
        region.height = (nri::Dim_t)h;
        region.depth = 1;
        NRI.CmdReadbackTextureToBuffer(cb, *readback, layout, *swap.texture, region);
    }
    to_target.before = to_target.after;
    to_target.after = {nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE};
    NRI.CmdBarrier(cb, barrier);
}

bool SwapOutput::Present(nri::CommandBuffer& cb, uint32_t frame_index, const Frame& frame) {
    (void)frame_index;
    auto& NRI = gfx_->NRI;
    nri::CommandBuffer* buffers[] = {&cb};
    nri::FenceSubmitDesc wait = {};
    wait.fence = textures_[frame.index].acquireSemaphore;
    wait.stages = nri::StageBits::COLOR_ATTACHMENT;
    nri::FenceSubmitDesc signal = {};
    signal.fence = frame.swap->releaseSemaphore;
    nri::QueueSubmitDesc submit = {};
    submit.waitFences = &wait;
    submit.waitFenceNum = 1;
    submit.commandBuffers = buffers;
    submit.commandBufferNum = 1;
    submit.signalFences = &signal;
    submit.signalFenceNum = 1;
    NRI.QueueSubmit(*gfx_->queue, submit);
    return NRI.QueuePresent(*swap_chain_, *frame.swap->releaseSemaphore, 0) == nri::Result::SUCCESS;
}

}  // namespace renderer::clips
