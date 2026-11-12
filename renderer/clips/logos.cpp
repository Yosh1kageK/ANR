#include "renderer/clips/logos.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "renderer/clips/project.h"
#include "renderer/paths.h"

namespace renderer::clips {
namespace fs = std::filesystem;

const char* corner_name(Corner corner) {
    switch (corner) {
        case Corner::BOTTOM_LEFT: return "bottom-left";
        case Corner::BOTTOM_RIGHT: return "bottom-right";
        case Corner::TOP_LEFT: return "top-left";
        case Corner::TOP_RIGHT: return "top-right";
    }
    return "bottom-left";
}

bool parse_corner(const std::string& name, Corner& corner) {
    for (Corner c : {Corner::BOTTOM_LEFT, Corner::BOTTOM_RIGHT, Corner::TOP_LEFT, Corner::TOP_RIGHT})
        if (name == corner_name(c)) {
            corner = c;
            return true;
        }
    return false;
}

std::vector<LogoItem> default_logos() {
    LogoItem apex;
    apex.id = "apex";
    apex.path = "apex.png";
    apex.corner = Corner::BOTTOM_LEFT;
    LogoItem nvidia;
    nvidia.id = "nvidia";
    nvidia.path = "nvidia.png";
    nvidia.corner = Corner::BOTTOM_RIGHT;
    return {apex, nvidia};
}

std::vector<LogoItem> LogoSettings::effective() const {
    return has_items ? items : default_logos();
}

std::vector<LogoItem>& LogoSettings::edit() {
    if (!has_items)
        items = default_logos();
    has_items = true;
    stored = true;
    items_dirty = true;
    return items;
}

LogoPlace place_logo(const LogoItem& item, int out_w, int out_h, double aspect, const std::vector<IntRect>& avoid) {
    LogoPlace place;
    place.backdrop = item.backdrop;
    const int h = std::max(1, (int)std::lround(item.height * out_h));
    const int w = std::max(1, (int)std::lround((double)h * aspect));
    const int margin = std::max(0, (int)std::lround(item.margin * out_h));
    const int pad = item.backdrop ? std::max(2, (int)std::lround(0.3 * h)) : 0;
    IntRect box;
    box.w = w + 2 * pad;
    box.h = h + 2 * pad;
    const bool right = item.corner == Corner::BOTTOM_RIGHT || item.corner == Corner::TOP_RIGHT;
    const bool bottom = item.corner == Corner::BOTTOM_LEFT || item.corner == Corner::BOTTOM_RIGHT;
    box.x = right ? out_w - margin - box.w : margin;
    box.y = bottom ? out_h - margin - box.h : margin;
    // Clear of the pills: along the vertical, away from the frame's edge, with a small gap. Each move can land on
    // another pill, so it repeats (at most once per pill).
    const int gap = std::max(2, margin / 2);
    for (size_t pass = 0; pass <= avoid.size(); pass++) {
        bool moved = false;
        for (const IntRect& a : avoid)
            if (box.intersects(a)) {
                box.y = bottom ? a.y - gap - box.h : a.y + a.h + gap;
                moved = true;
            }
        if (!moved)
            break;
    }
    box.y = std::clamp(box.y, 0, std::max(0, out_h - box.h));
    place.box = box;
    place.logo = {box.x + pad, box.y + pad, w, h};
    return place;
}

namespace {

double sinc(double x) {
    if (std::fabs(x) < 1e-9)
        return 1.0;
    const double p = 3.14159265358979323846 * x;
    return std::sin(p) / p;
}

double lanczos3(double x) {
    x = std::fabs(x);
    return x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
}

// The taps of one destination pixel: the first source index and the normalized weights.
struct Taps {
    int first = 0;
    std::vector<float> weights;
};

std::vector<Taps> make_taps(int source, int dest) {
    std::vector<Taps> taps((size_t)dest);
    const double ratio = (double)source / (double)dest;
    const double scale = std::max(ratio, 1.0);  // widened by the reduction
    const double support = 3.0 * scale;
    for (int d = 0; d < dest; d++) {
        const double centre = ((double)d + 0.5) * ratio - 0.5;
        const int lo = (int)std::floor(centre - support) + 1, hi = (int)std::floor(centre + support);
        Taps& t = taps[(size_t)d];
        t.first = lo;
        double sum = 0.0;
        std::vector<double> w;
        for (int i = lo; i <= hi; i++) {
            w.push_back(lanczos3(((double)i - centre) / scale));
            sum += w.back();
        }
        t.weights.reserve(w.size());
        for (double v : w)
            t.weights.push_back((float)(v / sum));
    }
    return taps;
}

}  // namespace

LogoImage resample_logo(const LogoImage& src, int dw, int dh) {
    LogoImage out;
    out.w = std::max(dw, 1);
    out.h = std::max(dh, 1);
    out.rgba.assign((size_t)out.w * out.h * 4, 0);
    if (src.w <= 0 || src.h <= 0 || src.rgba.size() < (size_t)src.w * src.h * 4)
        return out;
    // Premultiplied floats.
    std::vector<float> a((size_t)src.w * src.h * 4);
    for (size_t i = 0; i < (size_t)src.w * src.h; i++) {
        const float alpha = src.rgba[i * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; c++)
            a[i * 4 + c] = src.rgba[i * 4 + c] / 255.0f * alpha;
        a[i * 4 + 3] = alpha;
    }
    const std::vector<Taps> tx = make_taps(src.w, out.w), ty = make_taps(src.h, out.h);
    // Horizontal: src.h rows of out.w pixels.
    std::vector<float> b((size_t)out.w * src.h * 4, 0.0f);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < out.w; x++) {
            const Taps& t = tx[(size_t)x];
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.weights.size(); k++) {
                const int sx = std::clamp(t.first + (int)k, 0, src.w - 1);
                const float* p = &a[((size_t)y * src.w + sx) * 4];
                for (int c = 0; c < 4; c++)
                    acc[c] += p[c] * t.weights[k];
            }
            for (int c = 0; c < 4; c++)
                b[((size_t)y * out.w + x) * 4 + c] = acc[c];
        }
    // Vertical, then straight alpha.
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++) {
            const Taps& t = ty[(size_t)y];
            float acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < t.weights.size(); k++) {
                const int sy = std::clamp(t.first + (int)k, 0, src.h - 1);
                const float* p = &b[((size_t)sy * out.w + x) * 4];
                for (int c = 0; c < 4; c++)
                    acc[c] += p[c] * t.weights[k];
            }
            const float alpha = std::clamp(acc[3], 0.0f, 1.0f);
            uint8_t* o = &out.rgba[((size_t)y * out.w + x) * 4];
            o[3] = (uint8_t)std::lround(alpha * 255.0f);
            for (int c = 0; c < 3; c++) {
                const float premultiplied = std::clamp(acc[c], 0.0f, alpha);
                o[c] = alpha > 1e-6f ? (uint8_t)std::lround(std::clamp(premultiplied / alpha, 0.0f, 1.0f) * 255.0f) : 0;
            }
        }
    return out;
}

std::string find_logo_file(const std::string& path, const std::vector<std::string>& folders) {
    if (path.empty())
        return {};
    std::error_code ec;
    const bool absolute = path[0] == '~' || fs::path(path).is_absolute();
    if (absolute) {
        const std::string expanded = ExpandPath(path, "");
        return fs::is_regular_file(expanded, ec) ? expanded : std::string();
    }
    for (const std::string& folder : folders) {
        const std::string candidate = (fs::path(folder) / path).string();
        if (fs::is_regular_file(candidate, ec))
            return candidate;
    }
    return {};
}

std::vector<std::string> logo_folders(const std::string& project_folder) {
    std::vector<std::string> folders;
    if (const char* env = std::getenv("NR_LOGO_DIR"))
        if (*env)
            folders.push_back(env);
    for (const std::string& folder : paths::LogoFolders())
        folders.push_back(folder);
    if (!project_folder.empty())
        folders.push_back(project_folder);
    return folders;
}

std::string LogoFolderText() {
    std::string text;
    for (const std::string& folder : logo_folders(""))
        text += (text.empty() ? "" : ", ") + folder;
    return text;
}

}  // namespace renderer::clips
