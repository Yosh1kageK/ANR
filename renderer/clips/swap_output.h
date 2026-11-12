// The clip viewer's swap chain and the frame it presents: the choice among SDR (sRGB 8-bit), HDR10 (PQ 10-bit) and scRGB
// (FP16) by what the window's surface offers, the HDR metadata, the paper white and peak, and the UI on top: drawn
// into the swap chain at the paper white, or for HDR10 into its own linear layer that is blended in PQ code values by
// its coverage, so pixels without UI keep the video's codes exactly. The rules are the renderer's (README "Display and
// HDR"); the helpers it shares are in renderer/display.h. The renderer's own swap chain code (renderer.cpp
// CreateSwapChain and RenderFrame) keeps its members and is not built on this class, so that code is written twice.

#pragma once

#include <functional>
#include <optional>

#include "renderer/app.h"
#include "renderer/clips/gfx.h"
#include "renderer/display.h"

namespace renderer::clips {

// HDR metadata of the content (VK_EXT_hdr_metadata), for a swap chain whose content has its own mastering display: the
// study mode's per-trial values (BT.2020 primaries, D65 white). Without it the swap chain sends the peak and paper white.
struct SwapMetadata {
    float max_luminance = 0.0f, min_luminance = 0.0f, max_cll = 0.0f, max_fall = 0.0f;
    bool rec2020 = false;
};

struct SwapOptions {
    uint32_t min_textures = 0;  // at least this many swap chain images (FIFO presents were steadier with 4 on GNOME)
    bool vsync = true;
    bool hdr_metadata = true;
    uint32_t wanted = SRGB8;  // Encoding
    bool paper_white_set = false, peak_set = false;
    float paper_white = 0.0f, peak = 0.0f;
};

class SwapOutput {
public:
    ~SwapOutput() { Destroy(); }

    // The window's surface formats, the display's peak, and the paper white and peak. Call before Create.
    void QueryDisplay(Gfx& gfx, App& app, SDL_Window* window, const SwapOptions& options);
    // The surface's HDR formats again (the OS may have turned HDR on or off since QueryDisplay).
    void RequeryFormats();
    // Paper white and peak for the wanted mode, unless set (a mode change changes the PQ default).
    void UpdateDefaults(SDL_Window* window);
    // The swap chain at the window's pixel size (and the UI layer on HDR10). The wanted mode falls back to SDR.
    bool Create(Gfx& gfx, App& app, uint32_t queued_frames, std::string& error);
    void Destroy();

    uint32_t encoding() const { return encoding_; }
    uint32_t wanted() const { return wanted_; }
    void Want(uint32_t encoding) { wanted_ = encoding; }
    bool supported(uint32_t encoding) const { return supported_[encoding]; }
    nri::Format format() const { return format_; }
    bool hw_srgb() const { return !encode_srgb_; }  // an sRGB format encodes the shader's linear output itself
    bool dirty = false;                             // the swap chain must be created again (resize, mode change)
    HdrState hdr;
    bool hdr_metadata = true;
    std::optional<SwapMetadata> metadata;  // explicit metadata (HDR modes only)
    bool metadata_sent() const { return metadata_sent_; }  // the swap chain was created with HDR metadata
    uint32_t min_textures = 0;
    bool vsync = true;
    uint2 size() const { return size_; }

    struct Frame {
        SwapChainTexture* swap = nullptr;
        uint32_t index = 0;
    };
    // The next texture to draw into (false: the swap chain is out of date). frame_index: the frame's own number.
    bool Acquire(uint32_t frame_index, Frame& out);

    // Records the frame in `cb`: `video` draws the pictures into the swap chain target (the viewport is the window); then
    // the UI, if the app has one. Submits and presents.
    // readback: a buffer (rows of row_pitch bytes) to copy the finished frame into, for captures.
    void Record(nri::CommandBuffer& cb, nri::Streamer& streamer, const Frame& frame,
                const std::function<void(nri::Format format)>& video, bool ui, nri::Buffer* readback = nullptr,
                uint32_t row_pitch = 0);
    bool Present(nri::CommandBuffer& cb, uint32_t frame_index, const Frame& frame);

private:
    Gfx* gfx_ = nullptr;
    App* app_ = nullptr;
    uint32_t queued_ = 0;
    uint2 size_ = {0, 0};
    uint32_t encoding_ = SRGB8, wanted_ = SRGB8;
    bool supported_[ENCODINGS] = {true, false, false};
    nri::SwapChain* swap_chain_ = nullptr;
    std::vector<SwapChainTexture> textures_;
    nri::Format format_ = nri::Format::UNKNOWN;
    bool encode_srgb_ = true;  // the shader encodes sRGB itself (a UNORM swap chain format)
    bool metadata_sent_ = false;

    GfxResources ui_resources_;
    nri::Texture* ui_layer_ = nullptr;
    nri::Descriptor *ui_target_ = nullptr, *ui_view_ = nullptr;
    nri::DescriptorPool* ui_pool_ = nullptr;
    nri::DescriptorSet* ui_set_ = nullptr;
    nri::PipelineLayout* ui_layout_ = nullptr;
};

}  // namespace renderer::clips
