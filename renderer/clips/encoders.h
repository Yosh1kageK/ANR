// The video encoder of an export: a hardware encoder that works on this machine (found by trying it), else software.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace renderer::clips {

struct VideoEncoder {
    std::string name;                      // the ffmpeg encoder: "hevc_nvenc", "libx265", ...
    bool hardware = false;
    std::vector<std::string> global_args;  // before the input (a VAAPI device)
    std::string filter;                    // appended to the -vf chain ("" or "format=nv12,hwupload")
    std::vector<std::string> codec_args;   // -c:v ... and the rate settings
    bool set_pixel_format = true;          // -pix_fmt yuv420p (a hardware-upload filter sets the format itself)
    std::string note;                      // why software, when it is
};

// How an export spends bits.
//   QUALITY    constant quality: `crf` in libx265's scale (lower is better); each encoder gets its nearest setting
//   BITRATE    a target of `mbps` Mbit/s, peaks to 1.5 times it (variable bitrate with a 2-second buffer)
//   VERY_HIGH  a target from the output's pixels per second (VeryHighMbps) with each encoder's slowest, best preset
//   LOSSLESS   H.265 only: NVENC's lossless tuning, else libx265's lossless mode (of the 4:2:0 frames every mode encodes)
struct RateControl {
    enum Mode { QUALITY, BITRATE, VERY_HIGH, LOSSLESS } mode = QUALITY;
    int crf = 14;
    double mbps = 0.0;  // BITRATE: the target; VERY_HIGH: set from VeryHighMbps by the caller
};

// The very high target in Mbit/s for a w x h output at fps: 0.40 bits per pixel and frame at 60 fps for H.265 and 0.30
// for AV1 (AV1 needs about a quarter fewer bits for the same quality), times the square root of fps / 60 (frames closer
// in time differ less): 199 and 149 Mbit/s at 3840 x 2160 and 60 fps, 384 and 288 at 223 fps.
double VeryHighMbps(int codec, uint32_t w, uint32_t h, double fps);

// "crf 14", "bitrate 80 Mbit/s", "very high (199 Mbit/s)", "lossless": for logs and the export's JSON line.
std::string Describe(const RateControl& rate);

// codec: 0 H.265, 1 AV1 (no H.264). With allow_hardware the platform's hardware encoders are tried in order (a one-frame
// test encode each, cached per codec and rate mode for the process); a failure falls back to libx265 or libsvtav1.
VideoEncoder ChooseEncoder(int codec, const RateControl& rate, bool allow_hardware, const std::string& ffmpeg);

}  // namespace renderer::clips
