#include "renderer/clips/probe.h"

#include <cmath>
#include <cstdlib>

#include <nlohmann/json.hpp>
#include "renderer/clips/process.h"

namespace renderer::clips {
namespace {

using json = nlohmann::json;

std::string Text(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double Seconds(const json& j, const char* key) {
    const std::string s = Text(j, key);
    return s.empty() || s == "N/A" ? 0.0 : std::atof(s.c_str());
}

uint32_t BitDepth(const json& stream, const std::string& pix_fmt) {
    const std::string raw = Text(stream, "bits_per_raw_sample");
    if (!raw.empty() && raw != "N/A" && std::atoi(raw.c_str()) > 8)
        return (uint32_t)std::atoi(raw.c_str());
    for (uint32_t depth : {16u, 14u, 12u, 10u, 9u})
        if (pix_fmt.find(std::to_string(depth) + "le") != std::string::npos ||
            pix_fmt.find(std::to_string(depth) + "be") != std::string::npos)
            return depth;
    return 8;
}

}  // namespace

double ParseRate(const std::string& text) {
    const size_t slash = text.find('/');
    if (slash == std::string::npos)
        return std::atof(text.c_str());
    const double num = std::atof(text.substr(0, slash).c_str()), den = std::atof(text.substr(slash + 1).c_str());
    return den > 0.0 ? num / den : 0.0;
}

bool ParseProbe(const std::string& text, ProbeResult& result, std::string& error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("ffprobe's output is not JSON: ") + e.what();
        return false;
    }
    result = ProbeResult();
    const json* video = nullptr;
    if (j.contains("streams") && j["streams"].is_array())
        for (const json& s : j["streams"]) {
            const std::string type = Text(s, "codec_type");
            if (type == "audio")
                result.has_audio = true;
            if (type == "video" && !video) {
                // Cover art is a video stream too (attached_pic).
                const bool cover = s.contains("disposition") && s["disposition"].is_object() &&
                                   s["disposition"].value("attached_pic", 0) == 1;
                if (!cover)
                    video = &s;
            }
        }
    if (!video) {
        error = "no video stream";
        return false;
    }
    VideoInfo& info = result.info;
    info.width = (uint32_t)video->value("width", 0);
    info.height = (uint32_t)video->value("height", 0);
    info.codec = Text(*video, "codec_name");
    result.pix_fmt = Text(*video, "pix_fmt");
    if (info.width == 0 || info.height == 0) {
        error = "the video has no size";
        return false;
    }
    const double r = ParseRate(Text(*video, "r_frame_rate")), avg = ParseRate(Text(*video, "avg_frame_rate"));
    info.fps = r > 0.0 ? r : avg;
    if (info.fps <= 0.0) {
        error = "the video has no frame rate";
        return false;
    }
    result.variable_frame_rate = r > 0.0 && avg > 0.0 && std::fabs(r - avg) / r > 0.002;
    double duration = Seconds(*video, "duration");
    if (duration <= 0.0 && j.contains("format"))
        duration = Seconds(j["format"], "duration");
    result.duration = duration;
    const std::string nb = Text(*video, "nb_frames");
    const int64_t counted = nb.empty() || nb == "N/A" ? 0 : (int64_t)std::atoll(nb.c_str());
    info.frames = counted > 0 ? counted : (int64_t)std::llround(duration * info.fps);
    if (info.frames <= 0) {
        error = "the video has no frames";
        return false;
    }
    if (duration <= 0.0)
        result.duration = (double)info.frames / info.fps;
    info.bit_depth = BitDepth(*video, result.pix_fmt);
    const std::string space = Text(*video, "color_space"), range = Text(*video, "color_range");
    if (space == "bt709")
        info.matrix = YuvMatrix::BT709;
    else if (space == "smpte170m" || space == "bt470bg" || space == "smpte240m")
        info.matrix = YuvMatrix::BT601;
    else if (space == "bt2020nc" || space == "bt2020c")
        info.matrix = YuvMatrix::BT2020;
    else  // unspecified: HD is Rec. 709, SD Rec. 601
        info.matrix = info.height >= 720 || info.width >= 1280 ? YuvMatrix::BT709 : YuvMatrix::BT601;
    info.range = (range == "pc" || range == "jpeg" || result.pix_fmt.rfind("yuvj", 0) == 0) ? YuvRange::FULL
                                                                                          : YuvRange::LIMITED;
    return true;
}

bool ProbeVideo(const std::string& path, ProbeResult& result, std::string& error) {
    bool ok = false;
    const std::string out = RunCapture({FindTool("ffprobe", "NR_FFPROBE"), "-v", "error", "-print_format", "json",
                                        "-show_streams", "-show_format", path},
                                       &ok);
    if (!ok || out.empty()) {
        error = "ffprobe failed on " + path + " (is ffprobe installed? set NR_FFPROBE or put it on PATH)";
        return false;
    }
    return ParseProbe(out, result, error);
}

}  // namespace renderer::clips
