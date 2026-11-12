// The Captures tab's picture pass: one fullscreen draw (clips.slang still_fs, captures/stills.slang) that puts one or two
// still images (RGBA16F textures) into up to two regions of a target, or their difference. The same code serves the window
// and the offscreen composite of the image export.

#pragma once

#include <vector>

#include "renderer/clips/captures/capture_info.h"
#include "renderer/clips/gfx.h"
#include "renderer/clips/view_math.h"

namespace renderer::clips::captures {

enum class StillView { PICTURES, DIFF };  // (not DIFFERENCE: wingdi.h defines that name on Windows)

struct StillParams {
    Regions regions;
    StillView view = StillView::PICTURES;
    bool box_below_one = true;      // average the source pixels a screen pixel covers below a scale of 1 (else nearest)
    uint32_t target_encoding = 0;   // renderer/display.h Encoding: SRGB8, PQ10, SCRGB16
    float paper_white = 203.0f;
    bool hw_srgb = false;           // the target is an sRGB format that encodes itself
    Coding coding[2] = {Coding::SRGB8, Coding::SRGB8};
    uint32_t size[2][2] = {{1, 1}, {1, 1}};   // each image's width and height
    float steps[2] = {1023.0f, 1023.0f};      // UNORM steps of each source (0: floats)
    bool exists[2] = {true, true};
    float gap_gray = 0.06f;          // sRGB value where an image is missing
};

class StillPass {
public:
    ~StillPass() { Destroy(); }
    bool Create(Gfx& gfx, uint32_t queued_frames, std::string& error);
    void Destroy();
    // Records the draw inside the caller's rendering (a color attachment of `format`, viewport and scissor set). A missing
    // image may be null.
    void Record(nri::CommandBuffer& cb, uint32_t queued, nri::Format format, const StillParams& params, nri::Descriptor* a,
                nri::Descriptor* b);

private:
    Gfx* gfx_ = nullptr;
    uint64_t stride_ = 256;
    GfxResources resources_;
    nri::PipelineLayout* layout_ = nullptr;
    std::vector<nri::DescriptorSet*> sets_;
    std::vector<nri::Descriptor*> constant_views_;
    nri::Buffer* constants_ = nullptr;
    uint8_t* mapped_ = nullptr;
    nri::Descriptor* dummy_ = nullptr;
};

}  // namespace renderer::clips::captures
