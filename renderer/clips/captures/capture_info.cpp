#include "renderer/clips/captures/capture_info.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace renderer::clips::captures {

Coding CodingOf(const std::string& encoding) {
    if (encoding == "PQ10")
        return Coding::PQ10;
    if (encoding == "scRGB16")
        return Coding::SCRGB16;
    return Coding::SRGB8;
}

std::string GameName(const std::string& game_path) {
    std::string path = game_path;
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::string marker = "steamapps/common/";
    const size_t at = path.find(marker);
    if (at != std::string::npos) {
        const size_t begin = at + marker.size();
        const size_t end = path.find('/', begin);
        if (end != std::string::npos && end > begin)
            return path.substr(begin, end - begin);
    }
    const size_t slash = path.find_last_of('/');
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = file.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
}

int64_t ParseTime(const std::string& time) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(time.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6)
        return 0;
    // days from civil (Howard Hinnant's algorithm)
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

std::string DisplayTime(const std::string& time) {
    std::string text = time.substr(0, 16);
    std::replace(text.begin(), text.end(), 'T', ' ');
    return text;
}

std::vector<double> SlotTimes(int multiplier) {
    std::vector<double> times;
    for (int i = 1; i < multiplier; i++)
        times.push_back((double)i / multiplier);
    return times;
}

std::string FractionLabel(double t, int multiplier) {
    for (int i = 1; i < multiplier; i++)
        if (std::fabs(t - (double)i / multiplier) < 1e-3) {
            int n = i, m = multiplier;
            for (int g = std::min(n, m); g > 1; g--)
                if (n % g == 0 && m % g == 0) {
                    n /= g;
                    m /= g;
                    break;
                }
            return std::to_string(n) + "/" + std::to_string(m);
        }
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", t);
    return text;
}

std::string PairSpec::Stem() const {
    char text[96];
    std::snprintf(text, sizeof(text), "%s_f%d-f%d_t%.3f", kind == PairKind::REAL ? "real-pair" : "skip-one", a, b, t);
    return text;
}

std::string PairSpec::Label(int multiplier) const {
    char text[160];
    if (kind == PairKind::REAL)
        std::snprintf(text, sizeof(text), "frames %d and %d, slot %d (t = %s)", a, b, index, FractionLabel(t, multiplier).c_str());
    else
        std::snprintf(text, sizeof(text), "skip-one: frame %d from %d and %d (t = 1/2)", rendered, a, b);
    return text;
}

std::string PairSpec::Only() const {
    return kind == PairKind::REAL ? "real:" + std::to_string(b) + ":" + std::to_string(index) : "skip:" + std::to_string(rendered);
}

std::vector<PairSpec> EnumeratePairs(const CaptureInfo& c) {
    std::vector<PairSpec> pairs;
    const int n = (int)c.frames.size();
    for (int k = 1; k < n; k++) {
        if (c.frames[(size_t)k].after_gap)
            continue;
        for (const CallInfo& call : c.frames[(size_t)k].calls) {
            PairSpec p;
            p.kind = PairKind::REAL;
            p.a = k - 1;
            p.b = k;
            p.index = call.index;
            p.t = call.t;
            pairs.push_back(p);
        }
    }
    for (int k = 1; k <= n - 2; k++) {
        if (c.frames[(size_t)k].after_gap || c.frames[(size_t)k + 1].after_gap)
            continue;
        PairSpec p;
        p.kind = PairKind::SKIP_ONE;
        p.a = k - 1;
        p.b = k + 1;
        p.t = 0.5;
        p.rendered = k;
        pairs.push_back(p);
    }
    return pairs;
}

std::string TextureFile(int frame, const std::string& name) {
    return "f" + std::to_string(frame) + "_" + name + ".bin";
}

std::string GeneratedFile(int frame, int index) {
    return "f" + std::to_string(frame) + "_generated_" + std::to_string(index) + ".bin";
}

}  // namespace renderer::clips::captures
