// What ffprobe says about a recording: size, frame rate, frame count, codec, bit depth, the YUV matrix and range.
// ParseProbe is pure (tested by probe_test.cpp); ProbeVideo runs ffprobe.

#pragma once

#include <string>

#include "renderer/clips/video_source.h"

namespace renderer::clips {

struct ProbeResult {
    VideoInfo info;
    double duration = 0.0;          // seconds
    bool has_audio = false;
    bool variable_frame_rate = false;  // r_frame_rate and avg_frame_rate differ: frame numbers are not time / fps
    std::string pix_fmt;
};

// ffprobe -print_format json -show_streams -show_format output.
bool ParseProbe(const std::string& json_text, ProbeResult& result, std::string& error);
bool ProbeVideo(const std::string& path, ProbeResult& result, std::string& error);

// "60/1" or "60000/1001" to a rate; 0 when it is not one.
double ParseRate(const std::string& text);

}  // namespace renderer::clips
