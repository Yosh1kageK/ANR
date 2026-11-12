// View geometry: the study rules against the render test's origin() (tests/renderer/test_study_render.py), and the
// compare camera's exactness at integer scales.
#include "renderer/clips/view_math.h"

#include <cmath>
#include <vector>

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

// Python's origin() of the study test: floor division, the pan clamped when the zoomed video is larger.
int py_origin(int start, int area, int video, int zoom, int pan) {
    const int zoomed = video * zoom;
    const int centred = (int)std::floor((area - zoomed) / 2.0);
    if (zoomed > area)
        pan = std::min(std::max(pan, area - zoomed - centred), -centred);
    else
        pan = 0;
    return start + centred + pan;
}

void study_rules() {
    const int screens[] = {1920, 1081, 2560, 3840, 700, 33};
    const int videos[] = {1, 7, 900, 1920, 3840};
    const int pans[] = {-5000, -101, -1, 0, 1, 55, 4000};
    for (int screen : screens)
        for (int video : videos)
            for (int zoom : STUDY_ZOOMS)
                for (int pan : pans) {
                    CHECK_EQ(view_origin(0, screen, video, zoom, pan), py_origin(0, screen, video, zoom, pan));
                    CHECK_EQ(view_origin(100, screen, video, zoom, pan), py_origin(100, screen, video, zoom, pan));
                }
    // centre() is floor((screen - video) / 2) for negative differences too
    CHECK_EQ(centre(10, 13), -2);
    CHECK_EQ(centre(10, 14), -2);
    CHECK_EQ(centre(10, 9), 0);
    // side by side: panels of (S - 32) / 2 with a 32 pixel gap, centred
    CHECK_EQ(panel_width(1920), 944);
    CHECK_EQ(panel_left(1920), 0);
    CHECK_EQ(panel_right(1920), 944 + 32);
    CHECK_EQ(panel_width(1921), 944);
    CHECK_EQ(panel_left(1921), 0);
    CHECK_EQ(panel_left(1923), 0);  // (S - 32 - 2P) is 1: floor(1 / 2)
    CHECK_EQ(panel_left(31), floor_div(31 - 32, 2));
    // a zoom around a pixel keeps the source pixel under it
    for (int zoom : STUDY_ZOOMS)
        for (int next : STUDY_ZOOMS)
            for (int c : {0, 17, 500, 1919}) {
                const int screen = 1920, video = 900;
                const int pan0 = -37;
                const int d = c - view_origin(0, screen, video, zoom, pan0);
                const int source = floor_div(d, zoom);
                const int pan1 = zoom_pan(c, 0, screen, video, zoom, pan0, next);
                const int d1 = c - view_origin(0, screen, video, next, pan1);
                // when the clamp does not move the view, the same source pixel is under c
                if (clamp_pan(pan1, screen, video * next) == pan1 && d >= 0 && source < video)
                    CHECK_EQ(floor_div(d1, next), source);
            }
    // regions: slider and side by side
    StudyState s;
    s.view = StudyView::slider;
    s.split = 700;
    s.zoom = 3;
    s.pan_x = -101;
    s.pan_y = 55;
    Regions r = study_regions(s, 1920, 1080, 900, 500);
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.split, 700);
    CHECK_EQ(r.region[0].version, 2);
    CHECK_EQ((int)r.region[0].origin[0], py_origin(0, 1920, 900, 3, -101));
    CHECK_EQ((int)r.region[0].origin[1], py_origin(0, 1080, 500, 3, 55));
    CHECK_EQ(r.region[0].scale, 3.0f);
    s.view = StudyView::side_by_side;
    r = study_regions(s, 1920, 1080, 900, 500);
    CHECK_EQ(r.count, 2);
    CHECK_EQ(r.region[0].rect[0], 0);
    CHECK_EQ(r.region[0].rect[2], 944);
    CHECK_EQ(r.region[1].rect[0], 976);
    CHECK_EQ(r.region[1].version, 1);
    CHECK_EQ((int)r.region[1].origin[0], py_origin(976, 944, 900, 3, -101));
    s.view = StudyView::toggle;
    s.show_b = true;
    r = study_regions(s, 1920, 1080, 900, 500);
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.region[0].version, 1);
}

// The widths of the blocks one source row makes along a screen row of the area: every interior block of an integer
// scale is exactly `scale` pixels.
void exact_blocks(const Rect& area, int w, int h, Camera cam, double base) {
    const Mapping m = map_camera(area, w, h, cam, base);
    CHECK(m.nearest());
    if (std::floor(m.scale) != m.scale)
        return;
    CHECK(m.ox == std::floor(m.ox));  // whole screen pixels
    CHECK(m.oy == std::floor(m.oy));
    const int scale = (int)m.scale;
    // The runs of screen pixels that show the same source pixel along a row: all but the first and the last (cut by the
    // area's or the video's edge) are exactly `scale` pixels.
    std::vector<int> runs;
    int previous = -1;
    for (int x = (int)area.x; x < (int)(area.x + area.w); x++) {
        const double src = (x + 0.5 - m.ox) / m.scale;
        if (src < 0 || src >= w)
            continue;
        const int pixel = (int)std::floor(src);
        if (pixel != previous)
            runs.push_back(1);
        else
            runs.back()++;
        previous = pixel;
    }
    for (size_t i = 0; i < runs.size(); i++) {
        CHECK(runs[i] <= scale);
        if (i > 0 && i + 1 < runs.size())
            CHECK_EQ(runs[i], scale);
    }
}

void camera_exactness() {
    const Rect area = {0, 0, 1920, 1080};
    const int w = 3840, h = 2160;
    const double base = fit_scale(area, w, h);  // 0.5
    CHECK_NEAR(base, 0.5, 1e-12);
    // z = 1: the whole frame fits, centred, box filtered
    Mapping m = map_camera(area, w, h, Camera{0.5, 0.5, 1.0}, base);
    CHECK(!m.nearest());
    CHECK_NEAR(m.ox, 0.0, 1e-9);
    CHECK_NEAR(m.oy, 0.0, 1e-9);
    // z = 2 is 1:1 pixels, z = 4 is 2x, z = 8 is 4x, z = 16 is 8x: nearest, whole-pixel blocks, wherever the camera is
    for (double z : {2.0, 4.0, 8.0, 16.0})
        for (double cx : {0.0, 0.1234567, 0.5, 0.7777, 1.0})
            for (double cy : {0.0, 0.3333, 0.5, 0.91, 1.0})
                exact_blocks(area, w, h, Camera{cx, cy, z}, base);
    // another window shape and a video that does not fill it
    const Rect odd = {0, 0, 1357, 733};
    for (double z : {1.0, 2.0, 3.0, 5.0})
        for (double cx : {0.2, 0.5, 0.81})
            exact_blocks(odd, 640, 360, Camera{cx, 0.4, z}, 1.0);
    // a pan by whole pixels moves the picture by whole pixels at an integer scale
    Camera c{0.4, 0.45, 8.0};
    const Mapping m0 = map_camera(area, w, h, c, base);
    const Camera moved = pan_camera(area, w, h, c, base, 7.0, -3.0);
    const Mapping m1 = map_camera(area, w, h, moved, base);
    CHECK_NEAR(m1.ox - m0.ox, 7.0, 1e-9);
    CHECK_NEAR(m1.oy - m0.oy, -3.0, 1e-9);
    // below a scale of 1 nothing is snapped
    m = map_camera(area, w, h, Camera{0.5, 0.5, 1.5}, base);
    CHECK(!m.nearest());
}

void camera_clamps() {
    const Rect area = {0, 0, 1920, 1080};
    // zoomed in, the camera cannot leave the video: the picture still covers the area
    const Mapping m = map_camera(area, 3840, 2160, Camera{0.0, 1.0, 4.0}, 0.5);
    CHECK(m.ox <= 0.0 && m.ox + 3840 * m.scale >= 1920.0);
    CHECK(m.oy <= 0.0 && m.oy + 2160 * m.scale >= 1080.0);
    // zoomed out of a small video: centred
    const Mapping n = map_camera({0, 0, 1920, 1080}, 640, 360, Camera{0.1, 0.9, 1.0}, 1.0);
    CHECK_NEAR(n.ox, (1920 - 640) / 2.0, 1e-9);
    CHECK_NEAR(n.oy, (1080 - 360) / 2.0, 1e-9);
    // the zoom is limited to 1 .. 16
    CHECK_NEAR(clamp_camera(Camera{0.5, 0.5, 40.0}, area, 3840, 2160, 0.5).z, 16.0, 1e-12);
    CHECK_NEAR(clamp_camera(Camera{0.5, 0.5, 0.2}, area, 3840, 2160, 0.5).z, 1.0, 1e-12);
}

void zoom_around_cursor() {
    const Rect area = {0, 0, 1920, 1080};
    const int w = 3840, h = 2160;
    const double base = 0.5;
    Camera c{0.5, 0.5, 2.0};
    const double x = 1400.0, y = 300.0;
    const Mapping before = map_camera(area, w, h, c, base);
    double u0, v0;
    to_normalized(before, w, h, x, y, u0, v0);
    for (double next : {2.5, 3.0, 4.0, 7.3}) {
        const Camera z = zoom_camera(area, w, h, c, base, x, y, next);
        const Mapping after = map_camera(area, w, h, z, base);
        double u1, v1;
        to_normalized(after, w, h, x, y, u1, v1);
        // the same video point under the cursor, to within a screen pixel
        CHECK(std::fabs(u1 - u0) * w * after.scale <= 1.0);
        CHECK(std::fabs(v1 - v0) * h * after.scale <= 1.0);
        c = z;
    }
}

void layouts() {
    const Rect view = {0, 0, 1920, 1080};
    CompareLayout slider = compare_layout(Layout::slider, view, 0.25);
    CHECK(slider.slider);
    CHECK_EQ(slider.split, 480);
    Regions r = compare_regions(slider, 3840, 2160, Camera{0.5, 0.5, 1.0});
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.region[0].version, 2);
    CHECK_EQ(r.split, 480);
    CHECK_EQ(r.region[0].rect[2], 1920);
    CompareLayout sbs = compare_layout(Layout::side_by_side, view, 0.5);
    r = compare_regions(sbs, 3840, 2160, Camera{0.5, 0.5, 1.0});
    CHECK_EQ(r.count, 2);
    CHECK_EQ(r.region[0].version, 0);
    CHECK_EQ(r.region[1].version, 1);
    CHECK(r.region[0].rect[2] <= r.region[1].rect[0]);
    CHECK_EQ(r.region[0].rect[2] - r.region[0].rect[0], r.region[1].rect[2] - r.region[1].rect[0]);
    // the two panels frame the same part of their recordings
    CHECK_NEAR(r.region[0].scale, r.region[1].scale, 1e-9);
    CompareLayout only_b = compare_layout(Layout::b_only, view, 0.5);
    r = compare_regions(only_b, 3840, 2160, Camera{0.5, 0.5, 1.0});
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.region[0].version, 1);
    r = compare_regions(compare_layout(Layout::a_only, view, 0.5), 3840, 2160, Camera{0.5, 0.5, 1.0});
    CHECK_EQ(r.region[0].version, 0);
}

}  // namespace

int main() {
    study_rules();
    camera_exactness();
    camera_clamps();
    zoom_around_cursor();
    layouts();
    return check_result("view_math");
}
