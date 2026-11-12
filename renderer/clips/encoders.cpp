#include "renderer/clips/encoders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#include "renderer/clips/process.h"

namespace renderer::clips {
namespace {

std::vector<std::string> Backends() {
#if defined(__APPLE__)
    return {"videotoolbox"};
#elif defined(_WIN32)
    return {"nvenc", "qsv", "amf"};
#else
    return {"nvenc", "vaapi", "qsv"};
#endif
}

std::string Bits(double mbps) {
    return std::to_string((long long)std::llround(mbps * 1e6));
}

// The target, the peak (1.5 times) and a 2-second buffer of a bitrate mode.
std::vector<std::string> BitrateArgs(double mbps) {
    return {"-b:v", Bits(mbps), "-maxrate", Bits(mbps * 1.5), "-bufsize", Bits(mbps * 2.0)};
}

void Append(std::vector<std::string>& args, const std::vector<std::string>& more) {
    args.insert(args.end(), more.begin(), more.end());
}

// Empty codec_args: the backend has no such mode.
VideoEncoder Build(int codec, const std::string& backend, const RateControl& rate) {
    const std::string family = codec == 0 ? "hevc" : "av1";
    VideoEncoder e;
    e.name = family + "_" + backend;
    e.hardware = true;
    const bool by_rate = rate.mode == RateControl::BITRATE || rate.mode == RateControl::VERY_HIGH;
    const bool very_high = rate.mode == RateControl::VERY_HIGH;
    if (rate.mode == RateControl::LOSSLESS && (codec != 0 || backend != "nvenc"))
        return e;  // lossless: NVENC's H.265 only among the hardware encoders
    // NVENC and the others spend more bits at the same number: +11 (H.264) and +12 (AV1, not measured) land near the file size of
    // libx264 CRF 14 / libsvtav1 on game footage (measured for H.264 on an RTX 5090: see the commit message).
    const std::string q = std::to_string(std::clamp(codec == 0 ? rate.crf + 11 : rate.crf + 12, 0, 51));
    if (backend == "nvenc") {
        if (rate.mode == RateControl::LOSSLESS) {
            e.codec_args = {"-c:v", e.name, "-preset", "p7", "-tune", "lossless"};
        } else if (by_rate) {
            // Very high: the slowest preset with a full-resolution second pass, lookahead and both adaptive quantizers
            e.codec_args = {"-c:v", e.name, "-preset", very_high ? "p7" : "p5", "-tune", "hq", "-rc", "vbr"};
            Append(e.codec_args, BitrateArgs(rate.mbps));
            if (very_high)
                Append(e.codec_args, {"-multipass", "fullres", "-rc-lookahead", "32", "-spatial-aq", "1", "-temporal-aq", "1"});
            if (codec == 0)  // H.265's high tier: its levels allow these rates (main tier caps 4K at 25 to 60 Mbit/s)
                Append(e.codec_args, {"-tier", "high"});
        } else {
            e.codec_args = {"-c:v", e.name, "-preset", "p5", "-tune", "hq", "-rc", "vbr", "-cq", q, "-b:v", "0"};
        }
        if (codec == 0) {
            Append(e.codec_args, {"-profile:v", "main"});
            if (rate.mode != RateControl::LOSSLESS && !very_high)
                Append(e.codec_args, {"-spatial-aq", "1"});
        }
    } else if (backend == "videotoolbox") {
        e.codec_args = {"-c:v", e.name};
        if (by_rate)
            Append(e.codec_args, {"-b:v", Bits(rate.mbps), "-maxrate", Bits(rate.mbps * 1.5)});
        else
            Append(e.codec_args, {"-q:v", std::to_string(std::clamp(100 - rate.crf * 2, 1, 100))});
        Append(e.codec_args, {"-allow_sw", "0"});
    } else if (backend == "qsv") {
        e.codec_args = {"-c:v", e.name, "-preset", very_high ? "veryslow" : "medium"};
        if (by_rate)
            Append(e.codec_args, BitrateArgs(rate.mbps));
        else
            Append(e.codec_args, {"-global_quality", q});
    } else if (backend == "vaapi") {
        e.global_args = {"-vaapi_device", "/dev/dri/renderD128"};
        e.filter = "format=nv12,hwupload";
        e.set_pixel_format = false;
        e.codec_args = {"-c:v", e.name};
        if (by_rate) {
            Append(e.codec_args, {"-rc_mode", "VBR"});
            Append(e.codec_args, BitrateArgs(rate.mbps));
            if (very_high)
                Append(e.codec_args, {"-compression_level", "1"});  // the drivers' slowest, best setting
        } else {
            Append(e.codec_args, {"-qp", q});
        }
    } else {  // amf
        e.codec_args = {"-c:v", e.name, "-quality", "quality"};
        if (by_rate) {
            Append(e.codec_args, {"-rc", "vbr_peak"});
            Append(e.codec_args, BitrateArgs(rate.mbps));
            if (very_high)
                Append(e.codec_args, {"-preanalysis", "1"});
        } else {
            Append(e.codec_args, {"-rc", "cqp", "-qp_i", q, "-qp_p", q});
        }
    }
    if (codec == 0)  // the tag Apple's players and browsers need for HEVC in MP4
        Append(e.codec_args, {"-tag:v", "hvc1"});
    return e;
}

// A one-frame encode with the candidate: true when ffmpeg exits with 0.
bool Works(const std::string& ffmpeg, const VideoEncoder& e) {
    std::vector<std::string> args = {ffmpeg, "-nostdin", "-hide_banner", "-v", "error"};
    args.insert(args.end(), e.global_args.begin(), e.global_args.end());
    args.insert(args.end(), {"-f", "lavfi", "-i", "color=c=black:s=256x256:r=30", "-frames:v", "2"});
    std::string vf = "format=yuv420p";
    if (!e.filter.empty())
        vf += "," + e.filter;
    args.insert(args.end(), {"-vf", vf});
    args.insert(args.end(), e.codec_args.begin(), e.codec_args.end());
    args.insert(args.end(), {"-f", "null", "-"});
    bool ok = false;
    RunCapture(args, &ok);
    return ok;
}

}  // namespace

double VeryHighMbps(int codec, uint32_t w, uint32_t h, double fps) {
    const double bits_per_pixel = codec == 0 ? 0.40 : 0.30;
    const double rate = fps > 0.0 ? fps : 60.0;
    return bits_per_pixel * (double)w * (double)h * 60.0 * std::sqrt(rate / 60.0) / 1e6;
}

std::string Describe(const RateControl& rate) {
    char text[64];
    switch (rate.mode) {
        case RateControl::BITRATE: std::snprintf(text, sizeof(text), "bitrate %.0f Mbit/s", rate.mbps); break;
        case RateControl::VERY_HIGH: std::snprintf(text, sizeof(text), "very high (%.0f Mbit/s)", rate.mbps); break;
        case RateControl::LOSSLESS: std::snprintf(text, sizeof(text), "lossless"); break;
        default: std::snprintf(text, sizeof(text), "crf %d", rate.crf); break;
    }
    return text;
}

VideoEncoder ChooseEncoder(int codec, const RateControl& rate, bool allow_hardware, const std::string& ffmpeg) {
    static std::mutex mu;
    static std::map<std::pair<int, int>, std::string> working;  // (codec, mode) -> backend that works ("" none), per process
    std::string reason = "software encoding chosen";
    if (allow_hardware) {
        std::lock_guard<std::mutex> lock(mu);
        const std::pair<int, int> key = {codec, (int)rate.mode};
        auto found = working.find(key);
        if (found == working.end()) {
            std::string backend_ok;
            for (const std::string& backend : Backends()) {
                const VideoEncoder candidate = Build(codec, backend, rate);
                if (!candidate.codec_args.empty() && Works(ffmpeg, candidate)) {
                    backend_ok = backend;
                    break;
                }
            }
            found = working.emplace(key, backend_ok).first;
        }
        if (!found->second.empty())
            return Build(codec, found->second, rate);
        reason = "no hardware encoder worked for this codec and rate (tried the platform's ffmpeg encoders)";
    }
    VideoEncoder e;
    e.name = codec == 0 ? "libx265" : "libsvtav1";
    const bool by_rate = rate.mode == RateControl::BITRATE || rate.mode == RateControl::VERY_HIGH;
    if (codec == 0) {  // CRF 0 is lossless, as it was with libx264 (the export checks compare frames exactly)
        const bool lossless = rate.mode == RateControl::LOSSLESS || (rate.mode == RateControl::QUALITY && rate.crf == 0);
        e.codec_args = {"-c:v", "libx265", "-preset", rate.mode == RateControl::VERY_HIGH ? "slow" : "medium"};
        if (by_rate)
            Append(e.codec_args, BitrateArgs(rate.mbps));
        else if (!lossless)
            Append(e.codec_args, {"-crf", std::to_string(rate.crf)});
        Append(e.codec_args, {"-tag:v", "hvc1", "-x265-params", lossless ? "lossless=1:log-level=error" : "log-level=error"});
    } else {
        e.codec_args = {"-c:v", "libsvtav1", "-preset", rate.mode == RateControl::VERY_HIGH ? "4" : "6"};
        if (by_rate)
            Append(e.codec_args, {"-b:v", Bits(rate.mbps)});
        else
            Append(e.codec_args, {"-crf", std::to_string(std::min(rate.crf + 14, 63))});
    }
    e.note = reason;
    return e;
}

}  // namespace renderer::clips
