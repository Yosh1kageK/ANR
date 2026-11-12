// GPU video decode on NRI's video extension: a VideoSource (renderer/clips/video_source.h) that decodes an MP4 of AV1,
// H.264 or H.265 on the GPU's video decode queue and hands out the decoded frames as two sampled planes.
//
// Threads: the render thread calls the VideoSource methods; one decode thread reads the file, parses the bitstream,
// records and submits the decode and copy commands and waits on fences. The render thread never waits for the GPU.
// A frame is Ready once its copy into the output pool has completed on the graphics queue, which also means the
// decode finished. Pool textures stay in the shader resource layout; a frame's pixels change only after Release,
// when the pool reuses it, and the copy that overwrites it goes to the same graphics queue after the consumer's
// submission, so the consumer must have submitted every command that samples a frame before it calls Release.
//
// The device needs a video decode queue (nr::VulkanDevice::video_decode on Vulkan) obtained from NRI before the
// first Open; Open returns null with a reason when the container, codec, bit depth, driver, queue family or graphics
// API (Metal has no NRI video in this build) cannot be used, so the caller falls back to its software decoder.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "renderer/clips/video_source.h"

namespace renderer::clips {

struct NriVideoOptions {
    uint64_t memory_bytes = 1536ull << 20;  // decoded frames kept (the output pool); 4K 8-bit frames are 12 MiB
    uint32_t in_flight = 8;                 // samples being decoded or copied at once
};

class NriVideoSource final : public VideoSource {
public:
    // The contract's factory.
    static std::unique_ptr<VideoSource> Open(const VideoContext& context, const std::string& path,
                                             std::string* why_not);
    static std::unique_ptr<NriVideoSource> OpenWithOptions(const VideoContext& context, const std::string& path,
                                                           const NriVideoOptions& options, std::string* why_not);
    ~NriVideoSource() override;

    const VideoInfo& Info() const override;
    void SetInterest(int64_t first, int64_t last, int64_t playhead, int direction) override;
    bool Ready(int64_t frame) const override;
    bool Acquire(int64_t frame, FrameRef& out) override;
    void Release(const FrameRef& frame) override;
    DecodeStats Stats() const override;
    std::string Describe() const override;

    // Test support: the planes of a ready frame as the shaders see them (luma, then RG chroma; tightly packed rows),
    // read back on the graphics queue. Blocks. False if the frame is not ready.
    bool ReadFrame(int64_t frame, std::vector<uint8_t>* luma, std::vector<uint8_t>* chroma);
    // The first decode error, empty if none (the source then stops decoding).
    std::string Error() const;
    // Samples that start a GOP and the frame size of the pool, for the report.
    uint64_t frame_bytes() const;
    uint64_t pool_capacity() const;
    // CPU time the decode thread has used, in seconds.
    double DecodeThreadCpuSeconds() const;

private:
    struct Impl;
    explicit NriVideoSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace renderer::clips
