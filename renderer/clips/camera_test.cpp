// The area camera against the numbers the compare tool's earlier Python version (view_for, ordered_areas, camera_keys,
// camera_at, divider_at and caption_at) gives for the same areas (computed with the Python, which the web prototype used).
#include "renderer/clips/camera.h"

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

Area make(int id, const char* label, double t0, double t1, double x, double y, double w, double h,
          AreaDivider divider = {}) {
    Area a;
    a.id = id;
    a.label = label;
    a.t0 = t0;
    a.t1 = t1;
    a.rect[0] = x;
    a.rect[1] = y;
    a.rect[2] = w;
    a.rect[3] = h;
    a.divider = divider;
    return a;
}

}  // namespace

int main() {
    AreaDivider sweep;
    sweep.kind = AreaDivider::SWEEPING;
    sweep.sweeps = 1;  // the reference values are one swing
    const std::vector<Area> areas = {
        make(1, "bar counter", 30.0, 33.0, 0.406767955801105, 0.31767955801104975, 0.1864640883977901, 0.13812154696132595),
        make(2, "car", 33.5, 40.0, 0.8, 0.1, 0.2, 0.05, sweep),
        make(3, "late", 50.0, 52.0, 0.0, 0.0, 0.01, 0.01),
        make(4, "overlap", 38.0, 45.0, 0.3, 0.3, 0.3, 0.3),
    };

    // view_for: the rectangle with a margin of 15 %, the whole frame at most, 16x at most, kept inside the frame
    Camera v = view_for(areas[0].rect);
    CHECK_NEAR(v.cx, 0.5, 1e-9);
    CHECK_NEAR(v.cy, 0.3867403314917127, 1e-9);
    CHECK_NEAR(v.z, 4.663446054750401, 1e-9);
    v = view_for(areas[1].rect);
    CHECK_NEAR(v.cx, 0.885, 1e-9);
    CHECK_NEAR(v.cy, 0.125, 1e-9);
    CHECK_NEAR(v.z, 4.347826086956522, 1e-9);
    v = view_for(areas[2].rect);  // tiny: capped at 16x and kept inside
    CHECK_NEAR(v.cx, 0.03125, 1e-12);
    CHECK_NEAR(v.z, 16.0, 1e-12);
    const double whole[4] = {0.0, 0.0, 1.0, 1.0};
    CHECK_NEAR(view_for(whole).z, 1.0, 1e-12);

    // ordered_areas: by t0, each ending where the next starts
    const std::vector<Area> ordered = ordered_areas(areas);
    CHECK_EQ((int)ordered.size(), 4);
    CHECK_EQ(ordered[1].id, 2);
    CHECK_NEAR(ordered[1].t1, 38.0, 1e-12);  // trimmed by "overlap"
    CHECK_EQ(ordered[2].id, 4);
    CHECK_EQ(ordered[3].id, 3);
    std::vector<Area> empty = {make(9, "empty", 5.0, 5.0, 0, 0, 0.5, 0.5)};
    CHECK(ordered_areas(empty).empty());

    // camera_keys: ease in from FULL, hold, move straight between close areas, ease out
    const std::vector<CameraKey> keys = camera_keys(areas, 0.6);
    CHECK_EQ((int)keys.size(), 12);
    CHECK_NEAR(keys[0].first, 29.4, 1e-9);
    CHECK_NEAR(keys[0].second.z, 1.0, 1e-12);
    CHECK_NEAR(keys[3].first, 33.5, 1e-9);   // straight to the next area: it starts within twice the ease
    CHECK_NEAR(keys[7].first, 45.6, 1e-9);
    CHECK_NEAR(keys[8].first, 49.4, 1e-9);   // zoomed out between far apart areas
    CHECK_NEAR(keys[11].first, 52.6, 1e-9);
    CHECK(camera_keys({}, 0.6).empty());

    struct Case {
        double t, cx, cy, z, divider;
        const char* caption;
    } cases[] = {
        {0, 0.5, 0.5, 1.0, 0.3258, ""},
        {29.4, 0.5, 0.5, 1.0, 0.3258, ""},
        {29.7, 0.5, 0.443370166, 2.159501344, 0.3258, ""},
        {30.0, 0.5, 0.386740331, 4.663446055, 0.3258, "bar counter"},
        {31.5, 0.5, 0.386740331, 4.663446055, 0.3258, "bar counter"},
        {33.0, 0.5, 0.386740331, 4.663446055, 0.3258, ""},
        {33.2, 0.63552, 0.294607735, 4.549816801, 0.3258, ""},
        {33.5, 0.885, 0.125, 4.347826087, 0.5, "car"},
        {36.0, 0.885, 0.125, 4.347826087, 0.38029295, "car"},
        {39.0, 0.45, 0.45, 2.898550725, 0.3258, "overlap"},
        {45.0, 0.45, 0.45, 2.898550725, 0.3258, ""},
        {46.0, 0.5, 0.5, 1.0, 0.3258, ""},
        {49.8, 0.152777778, 0.152777778, 7.797236873, 0.3258, ""},
        {51.0, 0.03125, 0.03125, 16.0, 0.3258, "late"},
        {52.3, 0.265625, 0.265625, 4.0, 0.3258, ""},
        {60, 0.5, 0.5, 1.0, 0.3258, ""},
    };
    for (const Case& c : cases) {
        const Camera cam = camera_at(keys, c.t);
        CHECK_NEAR(cam.cx, c.cx, 1e-8);
        CHECK_NEAR(cam.cy, c.cy, 1e-8);
        CHECK_NEAR(cam.z, c.z, 1e-8);
        CHECK_NEAR(divider_at(0.3258, areas, c.t), c.divider, 1e-8);
        CHECK(caption_at(areas, c.t) == c.caption);
        const Shot shot = shot_at(areas, 0.6, 0.3258, c.t);
        CHECK_NEAR(shot.camera.z, c.z, 1e-8);
        CHECK(shot.caption == c.caption);
    }

    // a fixed divider holds its fraction while the area shows
    std::vector<Area> fixed = {make(1, "fixed", 1.0, 2.0, 0.1, 0.1, 0.2, 0.2)};
    fixed[0].divider.kind = AreaDivider::FIXED;
    fixed[0].divider.fraction = 0.8;
    CHECK_NEAR(divider_at(0.3, fixed, 1.5), 0.8, 1e-12);
    CHECK_NEAR(divider_at(0.3, fixed, 2.5), 0.3, 1e-12);
    return check_result("camera");
}
