// The renderer window's starting size: the requested size in pixels, in SDL's window coordinates (pixels divided by
// the pixel density: 2 on a Retina display or a 200% Wayland output, 1 on Windows, whose coordinates are pixels at any
// scale setting), scaled down with its aspect ratio kept until it and its decorations fit the display's usable area
// (the screen without task bars and docks). renderer/app.cpp applies it; window_fit_test.cpp checks it.

#pragma once

#include <algorithm>
#include <cmath>

namespace renderer {

struct WindowFit {
    int width = 0, height = 0;  // window coordinates (the client area, without decorations)
    bool clamped = false;       // smaller than requested
};

// usable_w/h: the display's usable bounds in window coordinates (0: unknown, no clamping); left/right/top/bottom:
// the decorations around the client area in window coordinates.
inline WindowFit FitWindow(int request_px_w, int request_px_h, float density, int usable_w, int usable_h, int left,
                           int right, int top, int bottom) {
    density = density > 0.0f ? density : 1.0f;
    const double w = std::max(1.0, std::ceil(request_px_w / (double)density)),
                 h = std::max(1.0, std::ceil(request_px_h / (double)density));
    WindowFit fit = {(int)w, (int)h, false};
    if (usable_w <= 0 || usable_h <= 0)
        return fit;
    const double max_w = std::max(1, usable_w - left - right), max_h = std::max(1, usable_h - top - bottom);
    const double scale = std::min({1.0, max_w / w, max_h / h});
    if (scale < 1.0) {
        fit.width = std::max(1, (int)std::floor(w * scale));
        fit.height = std::max(1, (int)std::floor(h * scale));
        fit.clamped = true;
    }
    return fit;
}

}  // namespace renderer
