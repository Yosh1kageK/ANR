// Where the videos go on the screen: pure functions (no GPU, no OS), tested by view_math_test.cpp. All screen values
// are in pixels; a source pixel is one pixel of the decoded frame.
//
// Two layers:
//
//  1. The study rules (renderer/README.md "Study mode"): integer zoom steps,
//     whole-pixel pans, the video centred by floor((area - video) / 2), a pan clamped so the video still covers the
//     area. A zoom of Z shows each source pixel as an exact Z x Z block.
//  2. The compare camera: (cx, cy, z) in the video's normalized coordinates, a continuous zoom from 1 to 16 around the
//     cursor, as the compare tool's web prototype had it. z = 1 shows the whole frame fitted to the
//     view area; the on-screen scale is fit * z. The mapping is snapped to whole screen pixels wherever the scale is
//     1 or more, which is where the shader samples nearest: at an integer scale every source pixel is an exact block.
//     Below 1 the shader averages the source pixels a screen pixel covers.
//
// Both layers end in a Mapping (screen position of source pixel 0's corner and the scale) per view area, which is
// all the shader needs.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace renderer::clips {

inline int floor_div(int a, int b) { return a >= 0 ? a / b : -((b - 1 - a) / b); }

// --- Layer 1: the study rules ----------------------------------------------------------------------------------------

enum class StudyView { toggle, slider, side_by_side };

inline constexpr int STUDY_GAP = 32;
inline constexpr int STUDY_ZOOMS[] = {1, 2, 3, 4, 6, 8, 12, 16};
inline constexpr int STUDY_MAX_ZOOM = STUDY_ZOOMS[std::size(STUDY_ZOOMS) - 1];

// Screen position of the first pixel when `video` pixels are centred in `screen` pixels: floor((screen - video) / 2).
inline int centre(int screen, int video) { return floor_div(screen - video, 2); }

inline int panel_width(int screen_w) { return (screen_w - STUDY_GAP) / 2; }
inline int panel_left(int screen_w) { return centre(screen_w, 2 * panel_width(screen_w) + STUDY_GAP); }
inline int panel_right(int screen_w) { return panel_left(screen_w) + panel_width(screen_w) + STUDY_GAP; }

// A pan moves the zoomed video by whole screen pixels from its centred place in a view area. It moves the video only
// while the video is larger than the area, and only as far as the video still covers the area.
inline int clamp_pan(int pan, int area, int zoomed) {
    if (zoomed <= area)
        return 0;
    const int c = centre(area, zoomed);
    return std::clamp(pan, area - zoomed - c, -c);
}

// Screen position of the top-left corner of source pixel 0 along one axis of a view area that starts at `start`.
inline int view_origin(int start, int area, int video, int zoom, int pan) {
    return start + centre(area, video * zoom) + clamp_pan(pan, area, video * zoom);
}

// The horizontal view area {start, width} at screen column x: the whole screen, or a side-by-side panel (the right one
// from its first column on).
inline std::pair<int, int> area_x(StudyView view, int screen_w, int x) {
    if (view != StudyView::side_by_side)
        return {0, screen_w};
    const int right = panel_right(screen_w);
    return {x >= right ? right : panel_left(screen_w), panel_width(screen_w)};
}

// The pan, not clamped, after zooming from `zoom` to `next` around screen position c of an area: the source pixel under
// c stays under it, at the same relative place in its block.
inline int zoom_pan(int c, int start, int area, int video, int zoom, int pan, int next) {
    const int d = c - view_origin(start, area, video, zoom, pan);
    const int source = floor_div(d, zoom);
    const int inside = d - source * zoom;
    const int origin = c - source * next - (2 * inside + 1) * next / (2 * zoom);
    return origin - start - centre(area, video * next);
}

// The study view's state: one mapping for both versions in every view.
struct StudyState {
    StudyView view = StudyView::toggle;
    bool show_b = false;  // toggle view
    int split = 0;        // slider: the first screen column that shows B
    int zoom = 1;         // one of STUDY_ZOOMS
    int pan_x = 0;        // screen pixels from the centred place; each view area clamps it
    int pan_y = 0;
};

// --- The mapping a draw takes ---------------------------------------------------------------------------------------

// A screen rectangle [x0, x1) x [y0, y1) in which screen pixel p shows source position (p + 0.5 - origin) / scale.
// version: 0 A, 1 B, 2 A left of the split column and B from it.
struct Region {
    int32_t rect[4] = {0, 0, 0, 0};
    float origin[2] = {0.0f, 0.0f};
    float scale = 1.0f;
    int32_t version = 0;
};

struct Regions {
    Regions() = default;
    int count = 0;
    int split = 0;  // the slider's first screen column that shows B
    Region region[2];
};

// The study views as regions.
inline Regions study_regions(const StudyState& s, int screen_w, int screen_h, int w, int h) {
    Regions r;
    r.split = std::clamp(s.split, 0, screen_w);
    const int y = view_origin(0, screen_h, h, s.zoom, s.pan_y);
    auto fill = [&](Region& region, int x0, int width, int version) {
        region.rect[0] = x0;
        region.rect[1] = 0;
        region.rect[2] = x0 + width;
        region.rect[3] = screen_h;
        region.origin[0] = (float)view_origin(x0, width, w, s.zoom, s.pan_x);
        region.origin[1] = (float)y;
        region.scale = (float)s.zoom;
        region.version = version;
    };
    if (s.view == StudyView::side_by_side) {
        const int panel = panel_width(screen_w);
        r.count = 2;
        fill(r.region[0], panel_left(screen_w), panel, 0);
        fill(r.region[1], panel_right(screen_w), panel, 1);
    } else {
        r.count = 1;
        fill(r.region[0], 0, screen_w, s.view == StudyView::slider ? 2 : int(s.show_b));
    }
    return r;
}

// --- Layer 2: the compare camera -------------------------------------------------------------------------------------

inline constexpr double MAX_ZOOM = 16.0;

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
};

// The centre of the view in the video's normalized coordinates, and the zoom (1: the whole frame fits the area).
struct Camera {
    double cx = 0.5, cy = 0.5, z = 1.0;
    bool operator==(const Camera& o) const { return cx == o.cx && cy == o.cy && z == o.z; }
};

struct Mapping {
    double scale = 1.0;         // screen pixels per source pixel
    double ox = 0.0, oy = 0.0;  // screen position of the top-left corner of source pixel 0
    Camera camera;              // the clamped camera this mapping came from
    bool nearest() const { return scale >= 1.0 - 1e-9; }
};

// The scale at which the whole frame fits the area (study views: 1 is the video's own pixels, base = 1).
inline double fit_scale(const Rect& area, int video_w, int video_h) {
    return std::min(area.w / video_w, area.h / video_h);
}

// Round half down: a half-pixel offset takes the lower pixel, as centre()'s floor does.
inline double snap(double v) { return std::ceil(v - 0.5); }

inline Camera clamp_camera(Camera c, const Rect& area, int video_w, int video_h, double base) {
    c.z = std::clamp(c.z, 1.0, MAX_ZOOM);
    const double s = base * c.z;
    const double ex = std::min(0.5, area.w / (2.0 * video_w * s)), ey = std::min(0.5, area.h / (2.0 * video_h * s));
    c.cx = video_w * s > area.w ? std::clamp(c.cx, ex, 1.0 - ex) : 0.5;
    c.cy = video_h * s > area.h ? std::clamp(c.cy, ey, 1.0 - ey) : 0.5;
    return c;
}

// The camera's mapping into a view area. base: the scale at z = 1 (fit_scale, or 1 for native pixels). From a scale of
// 1 the origin is snapped to whole screen pixels, so a nearest-sampled source pixel is a block of whole pixels and a
// pan moves the picture by whole pixels.
inline Mapping map_camera(const Rect& area, int video_w, int video_h, Camera camera, double base) {
    Mapping m;
    m.camera = clamp_camera(camera, area, video_w, video_h, base);
    m.scale = base * m.camera.z;
    m.ox = area.x + area.w / 2.0 - m.camera.cx * video_w * m.scale;
    m.oy = area.y + area.h / 2.0 - m.camera.cy * video_h * m.scale;
    if (m.scale >= 1.0 - 1e-9) {
        m.ox = snap(m.ox);
        m.oy = snap(m.oy);
    }
    return m;
}

inline void to_screen(const Mapping& m, int video_w, int video_h, double u, double v, double& x, double& y) {
    x = m.ox + u * video_w * m.scale;
    y = m.oy + v * video_h * m.scale;
}

inline void to_normalized(const Mapping& m, int video_w, int video_h, double x, double y, double& u, double& v) {
    u = (x - m.ox) / (video_w * m.scale);
    v = (y - m.oy) / (video_h * m.scale);
}

// Zoom to `next` around screen position (x, y): the video point under it stays under it (within the snap).
inline Camera zoom_camera(const Rect& area, int video_w, int video_h, Camera camera, double base, double x, double y,
                          double next) {
    const Mapping m = map_camera(area, video_w, video_h, camera, base);
    double u = 0.0, v = 0.0;
    to_normalized(m, video_w, video_h, x, y, u, v);
    Camera c = m.camera;
    c.z = std::clamp(next, 1.0, MAX_ZOOM);
    const double s = base * c.z;
    c.cx = u - (x - (area.x + area.w / 2.0)) / (video_w * s);
    c.cy = v - (y - (area.y + area.h / 2.0)) / (video_h * s);
    return clamp_camera(c, area, video_w, video_h, base);
}

// Pan by a screen delta (the picture follows the pointer).
inline Camera pan_camera(const Rect& area, int video_w, int video_h, Camera camera, double base, double dx, double dy) {
    Camera c = clamp_camera(camera, area, video_w, video_h, base);
    const double s = base * c.z;
    c.cx -= dx / (video_w * s);
    c.cy -= dy / (video_h * s);
    return clamp_camera(c, area, video_w, video_h, base);
}

// --- Compare layouts --------------------------------------------------------------------------------------------------

enum class Layout { slider, side_by_side, a_only, b_only };

inline int compare_gap(int width) { return std::max(4, width / 240); }

struct CompareLayout {
    Rect area[2];       // the view area of A and B (equal in the slider and single views)
    bool show[2] = {true, true};
    bool slider = false;
    int split = 0;      // slider: the first screen column that shows B
};

// The view areas in a window rectangle. divider: the slider's position as a fraction of the width.
inline CompareLayout compare_layout(Layout layout, const Rect& view, double divider) {
    CompareLayout l;
    if (layout == Layout::side_by_side) {
        const int gap = compare_gap((int)view.w);
        const double half = std::floor((view.w - gap) / 2.0);
        l.area[0] = {view.x, view.y, half, view.h};
        l.area[1] = {view.x + view.w - half, view.y, half, view.h};
        return l;
    }
    l.area[0] = l.area[1] = view;
    if (layout == Layout::slider) {
        l.slider = true;
        l.split = (int)std::lround(view.x + std::clamp(divider, 0.0, 1.0) * view.w);
    } else {
        l.show[layout == Layout::a_only ? 1 : 0] = false;
    }
    return l;
}

// The regions the video draw takes for a compare layout and the camera: side by side has two (each its own area, one
// shared camera), the slider one region that picks A or B by the split column, a single view one.
inline Regions compare_regions(const CompareLayout& l, int video_w, int video_h, Camera camera, bool fit = true) {
    Regions r;
    r.split = l.split;
    auto fill = [&](Region& region, const Rect& area, int version) {
        const double base = fit ? fit_scale(area, video_w, video_h) : 1.0;
        const Mapping m = map_camera(area, video_w, video_h, camera, base);
        region.rect[0] = (int)std::lround(area.x);
        region.rect[1] = (int)std::lround(area.y);
        region.rect[2] = (int)std::lround(area.x + area.w);
        region.rect[3] = (int)std::lround(area.y + area.h);
        region.origin[0] = (float)m.ox;
        region.origin[1] = (float)m.oy;
        region.scale = (float)m.scale;
        region.version = version;
    };
    if (l.slider) {
        r.count = 1;
        fill(r.region[0], l.area[0], 2);
    } else if (l.show[0] && l.show[1]) {
        r.count = 2;
        fill(r.region[0], l.area[0], 0);
        fill(r.region[1], l.area[1], 1);
    } else {
        r.count = 1;
        fill(r.region[0], l.show[0] ? l.area[0] : l.area[1], l.show[0] ? 0 : 1);
    }
    return r;
}

}  // namespace renderer::clips
