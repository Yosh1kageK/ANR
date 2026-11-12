// OpenVideo: the GPU decoder first (NRI's video extension, where the codec, container, driver and API allow it), else
// ffmpeg through a pipe. video_source.h has the contract; the GPU decoder is renderer/clips/video.

#include <mutex>

#include "renderer/clips/ffmpeg_source.h"
#include "renderer/clips/video/nri_video_source.h"
#include "renderer/clips/video_source.h"

namespace renderer::clips {
namespace {

std::mutex g_mutex;
FfmpegOptions g_ffmpeg_options;
bool g_force_ffmpeg = false;

}  // namespace

void SetVideoOptions(const FfmpegOptions& ffmpeg, bool force_ffmpeg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ffmpeg_options = ffmpeg;
    g_force_ffmpeg = force_ffmpeg;
}

std::unique_ptr<VideoSource> OpenVideo(const VideoContext& context, const std::string& path,
                                       std::string* gpu_decode_unavailable_because, uint64_t memory_bytes) {
    FfmpegOptions options;
    bool force_ffmpeg = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        options = g_ffmpeg_options;
        force_ffmpeg = g_force_ffmpeg;
    }
    if (memory_bytes)
        options.memory_bytes = memory_bytes;
    // GPU decode first, where the codec, container, driver and API allow it (it says why not otherwise).
    std::string reason;
    if (force_ffmpeg) {
        reason = "GPU decode is off (--decoder ffmpeg)";
    } else {
        NriVideoOptions gpu_options;
        gpu_options.memory_bytes = options.memory_bytes;
        std::unique_ptr<VideoSource> gpu = NriVideoSource::OpenWithOptions(context, path, gpu_options, &reason);
        if (gpu) {
            if (gpu_decode_unavailable_because)
                gpu_decode_unavailable_because->clear();
            return gpu;
        }
    }
    std::string error;
    std::unique_ptr<VideoSource> source = OpenFfmpegVideo(context, path, options, &error);
    if (gpu_decode_unavailable_because)
        *gpu_decode_unavailable_because = source ? reason : reason + "; ffmpeg: " + error;
    return source;
}

}  // namespace renderer::clips
