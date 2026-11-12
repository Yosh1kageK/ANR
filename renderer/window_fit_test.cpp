// renderer::FitWindow without a display: windows that fit, small and high-DPI screens, decorations, aspect ratio.
#include "renderer/window_fit.h"

#include <cstdio>

int main() {
    int failures = 0;
    auto expect = [&](const renderer::WindowFit& f, int w, int h, bool clamped, const char* what) {
        if (f.width != w || f.height != h || f.clamped != clamped) {
            std::printf("FAIL %s: %dx%d (clamped %d), want %dx%d\n", what, f.width, f.height, f.clamped, w, h);
            failures++;
        }
    };
    // 1920x1080 on a 2560x1600 laptop (task bar 48): fits as requested
    expect(renderer::FitWindow(1920, 1080, 1.0f, 2560, 1552, 1, 1, 31, 1), 1920, 1080, false, "fits");
    // 1366x768 screen, task bar 40, title bar 31: 16:9 kept inside 1364 x 696
    expect(renderer::FitWindow(1920, 1080, 1.0f, 1366, 728, 1, 1, 31, 1), 1237, 696, true, "1366x768");
    // 1280x720 with a 1920x1200 request (16:10)
    expect(renderer::FitWindow(1920, 1200, 1.0f, 1280, 680, 0, 0, 30, 0), 1040, 650, true, "1280x720, 16:10");
    // Retina / 200% Wayland: 3840x2160 pixels are 1920x1080 points; usable 1512x949 points
    expect(renderer::FitWindow(3840, 2160, 2.0f, 1512, 949, 0, 0, 28, 0), 1512, 850, true, "density 2");
    // Unknown usable area: as requested
    expect(renderer::FitWindow(4000, 3000, 1.0f, 0, 0, 0, 0, 0, 0), 4000, 3000, false, "no display bounds");
    if (!failures)
        std::printf("FitWindow: all cases pass\n");
    return failures ? 1 : 0;
}
