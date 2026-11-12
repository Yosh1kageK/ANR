// A VideoSource that decodes with the ffmpeg executable: it spawns `ffmpeg ... -f rawvideo -pix_fmt nv12 -` and reads
// frames from the pipe on a worker thread, straight into a mapped upload buffer, then copies each into a GPU texture
// slot. A seek restarts ffmpeg. The slots are the frame cache around the playhead (frame_cache.h): its size is the
// memory limit (default 1.5 GiB per video, in video memory), so steps back and short scrubs are instant. Software
// decoding in ffmpeg (a GPU decoder, NRI's video extension, comes first in OpenVideo where it is available).

#pragma once

#include <memory>
#include <string>

#include "renderer/clips/video_source.h"

namespace renderer::clips {

struct FfmpegOptions {
    uint64_t memory_bytes = 1536ull << 20;  // frame cache memory, per video
    int64_t preroll_frames = 12;            // a restart begins this far before the frame it needs
    int staging_buffers = 4;                // frames in flight between the pipe and the GPU
    std::string ffmpeg;                     // the executable (empty: NR_FFMPEG, next to ours, PATH)
};

// The options OpenVideo (video_source.h) uses for the ffmpeg source, and whether it skips the GPU decoder.
void SetVideoOptions(const FfmpegOptions& ffmpeg, bool force_ffmpeg);

// error: why not (ffprobe or ffmpeg missing, the file unreadable, no video memory).
std::unique_ptr<VideoSource> OpenFfmpegVideo(const VideoContext& context, const std::string& path,
                                             const FfmpegOptions& options, std::string* error);

}  // namespace renderer::clips
