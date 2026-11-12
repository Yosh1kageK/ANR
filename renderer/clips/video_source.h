// renderer/clips/video_source.h
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <NRIFramework.h>
namespace renderer::clips {
enum class YuvMatrix : uint8_t { BT709, BT601, BT2020 };
enum class YuvRange : uint8_t { LIMITED, FULL };
struct VideoInfo {
    uint32_t width = 0, height = 0;   // display size in pixels
    double fps = 0.0;                 // constant frame rate
    int64_t frames = 0;               // display frames
    std::string codec;                // "av1", "h264", "hevc"
    uint32_t bit_depth = 8;
    YuvMatrix matrix = YuvMatrix::BT709;
    YuvRange range = YuvRange::LIMITED;
    int64_t keyframe_gap = 0;         // typical frames between keyframes, 0 unknown
};
// One decoded display frame as two sampled planes: luma (R8 or R16 UNORM, full size) and chroma (RG8 or RG16 UNORM,
// half size, 4:2:0). Descriptors stay valid and pixels unchanged until Release. GPU-complete and sampleable by the
// graphics queue.
struct FrameRef { int64_t index = -1; nri::Descriptor* luma = nullptr; nri::Descriptor* chroma = nullptr; uint32_t slot = 0; };
struct DecodeStats { double decode_fps = 0; int64_t cached = 0; uint64_t cached_bytes = 0; int64_t decoded_total = 0; int64_t pending = 0; };
struct VideoContext { nri::Device* device = nullptr; const nri::CoreInterface* core = nullptr; const nri::HelperInterface* helper = nullptr; nri::Queue* graphics_queue = nullptr; /* decoder agent may extend */ };
class VideoSource {
public:
    virtual ~VideoSource() = default;
    virtual const VideoInfo& Info() const = 0;
    // Every frame, render thread: frames [first,last] are wanted; the playhead moves `direction` (+1, -1, 0) frames per
    // displayed frame. Decode them nearest the playhead first, ahead in the play direction; keep decoded frames within
    // the memory limit; never evict a pinned one.
    virtual void SetInterest(int64_t first, int64_t last, int64_t playhead, int direction) = 0;
    virtual bool Ready(int64_t frame) const = 0;
    virtual bool Acquire(int64_t frame, FrameRef& out) = 0;   // pins; false = not ready. Each success needs one Release.
    virtual void Release(const FrameRef& frame) = 0;
    virtual DecodeStats Stats() const = 0;
    virtual std::string Describe() const = 0;                 // e.g. "NRI Vulkan Video AV1, NVIDIA GeForce RTX 5090"
};
// memory_bytes: the decoded-frame cache's size (0: SetVideoOptions's).
std::unique_ptr<VideoSource> OpenVideo(const VideoContext& context, const std::string& path,
                                       std::string* gpu_decode_unavailable_because, uint64_t memory_bytes = 0);
}
