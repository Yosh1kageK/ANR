// The video pass: one fullscreen draw that puts one or two recordings (YUV planes from a VideoSource, or raw RGB
// textures) into up to two regions of a target (clips.slang video_fs and raw_fs). The same draw code serves the window,
// an offscreen target at another resolution (the export) and the study views.

#pragma once

#include "renderer/clips/gfx.h"
#include "renderer/clips/video_constants.h"
#include "renderer/clips/video_source.h"

namespace renderer::clips {

class VideoPass {
public:
    ~VideoPass() { Destroy(); }

    // queued_frames: how many frames the GPU can have in flight (constants and descriptors are per frame).
    bool Create(Gfx& gfx, uint32_t queued_frames, std::string& error);
    void Destroy();

    // Records the draw inside the caller's rendering (a color attachment of `format`, viewport and scissor set).
    // luma and chroma: the two recordings' planes; a missing one may be left null.
    void RecordVideo(nri::CommandBuffer& cb, uint32_t queued, nri::Format format, const VideoConstants& constants,
                     nri::Descriptor* const luma[2], nri::Descriptor* const chroma[2]);
    // Raw RGB pictures (the study's clips): textures of any RGB format the shader reads as float4.
    void RecordRaw(nri::CommandBuffer& cb, uint32_t queued, nri::Format format, const VideoConstants& constants,
                   nri::Descriptor* const picture[2]);

private:
    void WriteConstants(uint32_t queued, const VideoConstants& constants);

    Gfx* gfx_ = nullptr;
    uint32_t queued_ = 0;
    uint64_t stride_ = 256;
    GfxResources resources_;
    nri::PipelineLayout *video_layout_ = nullptr, *raw_layout_ = nullptr;
    std::vector<nri::DescriptorSet*> video_sets_, raw_sets_;
    std::vector<nri::Descriptor*> constant_views_;
    nri::Buffer* constants_ = nullptr;
    uint8_t* mapped_ = nullptr;
    nri::Descriptor *dummy_luma_ = nullptr, *dummy_chroma_ = nullptr, *dummy_picture_ = nullptr;
};

}  // namespace renderer::clips
