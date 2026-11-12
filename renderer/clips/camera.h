// The zoom camera of the marked areas: a port of the compare tool's earlier Python version (view_for, ordered_areas,
// camera_keys, camera_at, divider_at and caption_at of its web prototype), which the preview (Z) and the export both use. Pure
// (no GPU, no OS), tested by camera_test.cpp.
//
// The camera is (cx, cy, z): the centre of the view in the video's normalized coordinates, and the zoom (view_math.h).
// A span area is shown for t0 to t1 seconds on A's timeline: the camera eases in before t0 and out after t1, over `ease`
// seconds, with a smoothstep and a zoom that moves in log space. A freeze area (edit_list.h) stops the video at one frame
// and zooms to its rectangle and back with the same camera math; the functions below see the spans only.

#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "renderer/clips/view_math.h"

namespace renderer::clips {

inline constexpr double AREA_PAD = 1.15;     // an area's view is this much larger than its rectangle
inline constexpr double SWEEP = 0.35;        // a sweeping divider moves this far to each side of the middle
inline constexpr int DEFAULT_SWEEPS = 2;     // full back-and-forth swings of a sweeping divider over its window
inline constexpr double DEFAULT_EASE = 0.6;  // seconds of zoom in before an area's t0 and out after its t1
inline constexpr double DEFAULT_HOLD = 3.0;  // seconds a new span area is shown for
inline constexpr double DEFAULT_FREEZE_HOLD = 0.5;  // seconds a freeze area waits at its zoom

// How an area treats the divider: unchanged (the project's), sweeping, or fixed at a fraction of the width.
struct AreaDivider {
    enum Kind { KEEP, SWEEPING, FIXED } kind = KEEP;
    double fraction = 0.5;
    int sweeps = DEFAULT_SWEEPS;  // SWEEPING: full swings over the area's window (project files: "sweeps")
    bool operator==(const AreaDivider& o) const {
        return kind == o.kind && (kind != FIXED || fraction == o.fraction) && (kind != SWEEPING || sweeps == o.sweeps);
    }
    // The sweeping divider at x in [0, 1] of the window: sweeps full swings around the middle.
    double sweep_at(double x) const { return 0.5 + SWEEP * std::sin(2.0 * 3.14159265358979323846 * sweeps * x); }
};

// A span zooms while the video plays (t0 to t1). A freeze stops the video at the frame of t0, zooms to the rectangle,
// waits, zooms back and plays on (edit_list.h). Areas in project files without a kind are spans.
enum class AreaKind { SPAN, FREEZE };

struct Area {
    int id = 0;
    std::string label;
    double t0 = 0.0, t1 = 0.0;           // seconds on A's timeline (a freeze: t0 is the frozen frame's time, t1 = t0)
    double rect[4] = {0, 0, 0, 0};       // x, y, w, h normalized
    AreaDivider divider;
    AreaKind kind = AreaKind::SPAN;
    bool kind_stored = false;            // "kind" is written to the project: areas of older files have none and stay so
    std::optional<double> hold, ease;    // a freeze's own hold and zoom time in seconds (else the project's)
};

inline double smoothstep(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// The camera that shows a rectangle (x, y, w, h normalized) with a margin.
inline Camera view_for(const double rect[4], double pad = AREA_PAD, double max_zoom = MAX_ZOOM) {
    const double s = std::min(1.0, std::max(std::max(rect[2], rect[3]) * pad, 1.0 / max_zoom));
    Camera c;
    c.cx = std::clamp(rect[0] + rect[2] / 2.0, s / 2.0, 1.0 - s / 2.0);
    c.cy = std::clamp(rect[1] + rect[3] / 2.0, s / 2.0, 1.0 - s / 2.0);
    c.z = 1.0 / s;
    return c;
}

// The span areas by t0, each ending no later than the next one starts; empty ones and freezes left out.
inline std::vector<Area> ordered_areas(const std::vector<Area>& areas) {
    std::vector<Area> out;
    for (const Area& a : areas)
        if (a.kind == AreaKind::SPAN && a.t1 > a.t0)
            out.push_back(a);
    std::stable_sort(out.begin(), out.end(), [](const Area& a, const Area& b) { return a.t0 < b.t0; });
    for (size_t i = 0; i + 1 < out.size(); i++)
        out[i].t1 = std::min(out[i].t1, out[i + 1].t0);
    out.erase(std::remove_if(out.begin(), out.end(), [](const Area& a) { return !(a.t1 > a.t0); }), out.end());
    return out;
}

using CameraKey = std::pair<double, Camera>;

// Camera keys (time, camera). Between two keys the camera moves with a smoothstep; equal keys hold it. After an area it
// zooms out over `ease` seconds, unless the next area starts within twice that: then it moves straight there.
inline std::vector<CameraKey> camera_keys(const std::vector<Area>& areas, double ease = DEFAULT_EASE) {
    const Camera full;
    std::vector<CameraKey> keys;
    bool have_previous = false;
    double previous_end = 0.0;
    for (const Area& a : ordered_areas(areas)) {
        const Camera view = view_for(a.rect);
        if (!have_previous) {
            keys.push_back({a.t0 - ease, full});
        } else if (a.t0 - previous_end >= 2.0 * ease) {
            keys.push_back({previous_end + ease, full});
            keys.push_back({a.t0 - ease, full});
        }
        keys.push_back({a.t0, view});
        keys.push_back({a.t1, view});
        have_previous = true;
        previous_end = a.t1;
    }
    if (have_previous)
        keys.push_back({previous_end + ease, full});
    return keys;
}

// Between two cameras with a smoothstep weight u (0: a, 1: b): the centre moves linearly, the zoom in log space.
inline Camera lerp_camera(const Camera& a, const Camera& b, double u) {
    if (u <= 0.0)
        return a;
    if (u >= 1.0)
        return b;
    return {a.cx + (b.cx - a.cx) * u, a.cy + (b.cy - a.cy) * u, std::exp(std::log(a.z) + (std::log(b.z) - std::log(a.z)) * u)};
}

inline Camera camera_at(const std::vector<CameraKey>& keys, double t) {
    const Camera full;
    if (keys.empty() || t <= keys.front().first)
        return full;
    if (t >= keys.back().first)
        return keys.back().second;
    size_t lo = 0, hi = keys.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (keys[mid].first <= t)
            lo = mid;
        else
            hi = mid;
    }
    const auto& [t0, a] = keys[lo];
    const auto& [t1, b] = keys[hi];
    if (t1 <= t0)
        return b;
    return lerp_camera(a, b, smoothstep((t - t0) / (t1 - t0)));
}

// The divider as a fraction of the width at A time t: the area's (sweeping or fixed) while one is shown, else the
// project's.
inline double divider_at(double project_divider, const std::vector<Area>& areas, double t) {
    for (const Area& a : ordered_areas(areas))
        if (a.t0 <= t && t < a.t1) {
            if (a.divider.kind == AreaDivider::SWEEPING)
                return a.divider.sweep_at((t - a.t0) / (a.t1 - a.t0));
            if (a.divider.kind == AreaDivider::FIXED)
                return a.divider.fraction;
        }
    return project_divider;
}

inline std::string caption_at(const std::vector<Area>& areas, double t) {
    for (const Area& a : ordered_areas(areas))
        if (a.t0 <= t && t < a.t1)
            return a.label;
    return {};
}

// The camera, divider and caption at one time of A's timeline, as the preview and the export show them.
struct Shot {
    Camera camera;
    double divider = 0.5;
    std::string caption;
};

inline Shot shot_at(const std::vector<Area>& areas, double ease, double project_divider, double t) {
    return {camera_at(camera_keys(areas, ease), t), divider_at(project_divider, areas, t), caption_at(areas, t)};
}

}  // namespace renderer::clips
