// The offscreen composite that the video export and the Captures tab's image export share: a target at the output size,
// the overlay's draw list (labels, divider, caption, logos: overlay.h, logo_layer.h) drawn over the pictures by its own NRI
// ImGui instance, and the readback into host memory. The caller draws the pictures (a video pass, a stills pass) inside the
// rendering; the rest is here. Slots let several frames be in flight (the video export pipes them to an encoder).
//
// An SDR target (RGBA8_UNORM) takes the overlay directly. An HDR one (RGBA16_UNORM holding PQ codes) takes it through a
// layer blended in PQ code values by coverage at the paper white, as the window's HDR10 swap chain does
// (swap_output.cpp), so the pixels outside the overlay keep the pictures' codes exactly.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include "renderer/clips/gfx.h"

namespace renderer::clips {

class OffscreenComposer {
public:
    ~OffscreenComposer() { Destroy(); }

    // format: RGBA8_UNORM (SDR) or RGBA16_UNORM (HDR: PQ codes, the overlay at `paper_white`). Needs an ImGui context with a
    // font (the overlay's text) and the Gfx's descriptor pool.
    bool Create(Gfx& gfx, uint32_t width, uint32_t height, nri::Format format, uint32_t slots, std::string& error,
                float paper_white = 203.0f);
    void Destroy();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t row_pitch() const { return row_pitch_; }
    uint32_t bytes_per_pixel() const { return bytes_per_pixel_; }
    nri::Format format() const { return format_; }

    // The overlay's list for the next frame: reset, the font atlas current, clipped to the target. Call inside the ImGui
    // frame (the text is laid out with its font), draw the overlay into it, then Record after ImGui::Render.
    ImDrawList* BeginOverlay();
    // Records and submits the frame of `slot`: clear, `pictures` (in the rendering, viewport and scissor set), the overlay,
    // the copy into the slot's readback buffer.
    void Record(uint32_t slot, const std::function<void(nri::CommandBuffer&)>& pictures);
    // Blocks until the GPU is done with `slot`; its pixels (rows of row_pitch bytes) are then readable.
    void Wait(uint32_t slot);
    const uint8_t* Pixels(uint32_t slot) const { return slots_[slot].data; }

private:
    struct Slot {
        nri::CommandAllocator* allocator = nullptr;
        nri::CommandBuffer* command_buffer = nullptr;
        nri::Buffer* buffer = nullptr;
        nri::Memory* memory = nullptr;
        uint8_t* data = nullptr;
        uint64_t fence_value = 0;
    };

    Gfx* gfx_ = nullptr;
    uint32_t width_ = 0, height_ = 0, row_pitch_ = 0, bytes_per_pixel_ = 4;
    float paper_white_ = 203.0f;
    nri::Format format_ = nri::Format::RGBA8_UNORM;
    GfxResources resources_;
    nri::Texture* target_ = nullptr;
    nri::Descriptor* target_view_ = nullptr;
    std::vector<Slot> slots_;
    nri::Fence* fence_ = nullptr;
    uint64_t submitted_ = 0;
    nri::Streamer* streamer_ = nullptr;
    nri::ImguiInterface imgui_interface_ = {};
    nri::Imgui* imgui_ = nullptr;
    std::unique_ptr<ImDrawList> list_;
    // HDR: the overlay's layer (linear, premultiplied) and the pass that blends it into the PQ codes
    GfxResources layer_resources_;
    nri::Texture* layer_ = nullptr;
    nri::Descriptor *layer_target_ = nullptr, *layer_view_ = nullptr;
    nri::PipelineLayout* layer_layout_ = nullptr;
    nri::DescriptorSet* layer_set_ = nullptr;
};

}  // namespace renderer::clips
