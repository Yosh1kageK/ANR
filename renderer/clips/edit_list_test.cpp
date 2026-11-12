// The edit list: output length, the output frame to pair mapping, camera continuity at every boundary, and the rules for
// freezes inside spans, close together, at the ends and outside the range (edit_list.h).
#include "renderer/clips/edit_list.h"

#include <cmath>

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

Area freeze(int id, double frame, double fps, double x, double y, double w, double h, const char* label = "") {
    Area a;
    a.id = id;
    a.label = label;
    a.kind = AreaKind::FREEZE;
    a.kind_stored = true;
    a.t0 = a.t1 = frame / fps;
    a.rect[0] = x;
    a.rect[1] = y;
    a.rect[2] = w;
    a.rect[3] = h;
    return a;
}

Area span(int id, double t0, double t1, double x, double y, double w, double h, const char* label = "") {
    Area a;
    a.id = id;
    a.label = label;
    a.t0 = t0;
    a.t1 = t1;
    a.rect[0] = x;
    a.rect[1] = y;
    a.rect[2] = w;
    a.rect[3] = h;
    return a;
}

EditOptions options(int64_t first, int64_t end) {
    EditOptions o;
    o.fps = 60.0;
    o.first = first;
    o.end = end;
    o.ease = 0.5;         // 30 frames
    o.freeze_hold = 0.5;  // 30 frames
    o.divider = 0.3;
    return o;
}

bool same(const Camera& a, const Camera& b, double tolerance = 1e-12) {
    return std::fabs(a.cx - b.cx) <= tolerance && std::fabs(a.cy - b.cy) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

void no_marks() {
    const EditList list({}, options(100, 400));
    CHECK_EQ(list.length(), 300);
    for (int64_t i = 0; i < 300; i++) {
        const EditEntry e = list.at(i);
        CHECK_EQ(e.pair, 100 + i);
        CHECK(!e.frozen && e.freeze_area == 0);
        CHECK(same(e.shot.camera, Camera()));
    }
    CHECK_EQ(list.first_output_of(250), 150);
    // spans alone: the shots are shot_at's
    const std::vector<Area> spans = {span(1, 2.0, 3.0, 0.2, 0.2, 0.2, 0.2, "cap")};
    const EditList with_span(spans, options(0, 300));
    CHECK_EQ(with_span.length(), 300);
    for (int64_t i = 0; i < 300; i += 7) {
        const Shot s = shot_at(spans, 0.5, 0.3, (double)i / 60.0);
        CHECK(same(with_span.at(i).shot.camera, s.camera));
        CHECK(with_span.at(i).shot.caption == s.caption);
        CHECK_NEAR(with_span.at(i).shot.divider, s.divider, 1e-12);
    }
}

void one_freeze() {
    // 100 source frames from 1000; a freeze at 1040: ease 30, hold 30, ease 30: 90 more frames
    const std::vector<Area> areas = {freeze(1, 1040, 60.0, 0.4, 0.4, 0.1, 0.1, "detail")};
    const EditList list(areas, options(1000, 1100));
    CHECK_EQ(list.freezes().size(), (size_t)1);
    CHECK_EQ(list.freezes()[0].length(), 90);
    CHECK_EQ(list.length(), 190);
    CHECK_EQ(list.first_output_of(1039), 39);
    CHECK_EQ(list.first_output_of(1040), 40);
    CHECK_EQ(list.first_output_of(1041), 131);
    const Camera view = view_for(areas[0].rect);
    for (int64_t i = 0; i < 40; i++) {  // up to the frozen frame: plain
        CHECK_EQ(list.at(i).pair, 1000 + i);
        CHECK(!list.at(i).frozen);
    }
    for (int64_t j = 0; j <= 90; j++) {  // the sequence: pair 1040 for steps 0 to 90
        const EditEntry e = list.at(40 + j);
        CHECK_EQ(e.pair, 1040);
        CHECK_EQ(e.step, j);
        CHECK_EQ(e.freeze_area, 1);
        CHECK(e.frozen == (j > 0));
        CHECK(e.shot.caption == (j >= 30 && j <= 60 ? "detail" : ""));
    }
    CHECK(same(list.at(40).shot.camera, Camera()));       // step 0: the base
    CHECK(same(list.at(40 + 30).shot.camera, view));      // zoomed in
    CHECK(same(list.at(40 + 45).shot.camera, view));      // holding
    CHECK(same(list.at(40 + 60).shot.camera, view));
    CHECK(same(list.at(40 + 90).shot.camera, Camera()));  // back
    CHECK(list.at(40 + 15).shot.camera.z > 1.0 && list.at(40 + 15).shot.camera.z < view.z);
    CHECK(list.at(40 + 75).shot.camera.z > 1.0 && list.at(40 + 75).shot.camera.z < view.z);
    // symmetric: the zoom out mirrors the zoom in
    CHECK_NEAR(list.at(40 + 15).shot.camera.z, list.at(40 + 75).shot.camera.z, 1e-9);
    for (int64_t i = 131; i < 190; i++) {  // after it: pair 1041 on
        CHECK_EQ(list.at(i).pair, 1041 + (i - 131));
        CHECK(!list.at(i).frozen && list.at(i).freeze_area == 0);
        CHECK(same(list.at(i).shot.camera, Camera()));
    }
    CHECK_EQ(list.at(189).pair, 1099);
    // out of range indices clamp
    CHECK_EQ(list.at(-5).pair, 1000);
    CHECK_EQ(list.at(100000).pair, 1099);
    // the phases
    const FreezeSequence& f = list.freezes()[0];
    CHECK(freeze_phase(f, 0) == FreezePhase::ZOOMING_IN && freeze_phase(f, 29) == FreezePhase::ZOOMING_IN);
    CHECK(freeze_phase(f, 30) == FreezePhase::HOLDING && freeze_phase(f, 60) == FreezePhase::HOLDING);
    CHECK(freeze_phase(f, 61) == FreezePhase::ZOOMING_OUT && freeze_phase(f, 90) == FreezePhase::ZOOMING_OUT);
}

// The camera never jumps: between neighbouring output frames it moves no more than the smoothstep's steepest step.
void continuity() {
    const std::vector<Area> areas = {freeze(1, 40, 60.0, 0.7, 0.1, 0.1, 0.1), freeze(2, 41, 60.0, 0.1, 0.6, 0.2, 0.2),
                                     freeze(3, 90, 60.0, 0.3, 0.3, 0.1, 0.1), freeze(4, 90, 60.0, 0.5, 0.5, 0.2, 0.2)};
    for (bool with_span : {false, true}) {
        std::vector<Area> all = areas;
        if (with_span)
            all.push_back(span(9, 0.5, 1.4, 0.2, 0.2, 0.3, 0.3, "span"));
        const EditList list(all, options(0, 150));
        double worst = 0.0;
        for (int64_t i = 1; i < list.length(); i++) {
            const Camera a = list.at(i - 1).shot.camera, b = list.at(i).shot.camera;
            const double dz = std::fabs(std::log(b.z) - std::log(a.z)), dx = std::fabs(b.cx - a.cx), dy = std::fabs(b.cy - a.cy);
            worst = std::max({worst, dz, dx, dy});
            // the divider moves smoothly too
            CHECK(std::fabs(list.at(i).shot.divider - list.at(i - 1).shot.divider) < 0.2);
        }
        // a zoom of 16x over 30 frames moves at most 1.5 * log(16) / 30 per frame (the smoothstep's peak slope)
        CHECK(worst <= 1.5 * std::log(16.0) / 30.0 + 1e-9);
    }
    // at the boundaries of one sequence the camera is the base's, exactly, with no span to move it
    const EditList one({freeze(1, 40, 60.0, 0.7, 0.1, 0.1, 0.1)}, options(0, 150));
    CHECK(same(one.at(39).shot.camera, one.at(40).shot.camera, 0.0));
    CHECK(same(one.at(40 + 90).shot.camera, one.at(40 + 91).shot.camera, 0.0));
    // the slope at the hold's two ends is 0 (smoothstep): the first step in is much smaller than the one in the middle
    const double first_step = std::fabs(std::log(one.at(41).shot.camera.z) - std::log(one.at(40).shot.camera.z));
    const double middle_step = std::fabs(std::log(one.at(40 + 16).shot.camera.z) - std::log(one.at(40 + 15).shot.camera.z));
    CHECK(first_step < 0.1 * middle_step);
}

void same_frame_and_close_marks() {
    // two marks at one frame run one after the other; the second starts from the base camera
    const std::vector<Area> same_frame = {freeze(1, 50, 60.0, 0.1, 0.1, 0.1, 0.1, "one"), freeze(2, 50, 60.0, 0.6, 0.6, 0.1, 0.1, "two")};
    const EditList list(same_frame, options(0, 120));
    CHECK_EQ(list.length(), 120 + 180);
    CHECK_EQ(list.freezes().size(), (size_t)2);
    CHECK_EQ(list.freezes()[0].area_id, 1);  // the list's order at one frame
    CHECK(same(list.at(50).shot.camera, Camera()));
    CHECK(same(list.at(50 + 90).shot.camera, Camera()));   // the first one's last step
    CHECK(list.at(50 + 91).shot.camera.z < 1.01);           // the second one starts from the base: a small first step
    CHECK_EQ(list.at(50 + 91).freeze_area, 2);
    CHECK_EQ(list.at(50 + 91).step, 1);
    CHECK(list.at(50 + 90 + 30).shot.caption == "two");
    CHECK(list.at(50 + 30).shot.caption == "one");
    CHECK_EQ(list.at(50 + 180).pair, 50);
    CHECK_EQ(list.at(50 + 181).pair, 51);
    CHECK_EQ(list.first_output_of(51), 50 + 181);
    // marks 5 frames apart: the first runs to its end, five plain frames follow, then the second
    const std::vector<Area> close = {freeze(1, 50, 60.0, 0.1, 0.1, 0.1, 0.1), freeze(2, 55, 60.0, 0.6, 0.6, 0.1, 0.1)};
    const EditList c(close, options(0, 120));
    CHECK_EQ(c.length(), 120 + 180);
    CHECK_EQ(c.at(50 + 90).pair, 50);
    CHECK_EQ(c.at(50 + 91).pair, 51);
    CHECK_EQ(c.at(50 + 95).pair, 55);
    CHECK_EQ(c.at(50 + 95).step, 0);
    CHECK_EQ(c.at(50 + 95).freeze_area, 2);
    CHECK(same(c.at(50 + 95).shot.camera, Camera()));
    CHECK_EQ(c.at(50 + 95 + 90).pair, 55);
    CHECK_EQ(c.at(50 + 95 + 91).pair, 56);
    // the order follows the frames, not the list
    const std::vector<Area> reversed = {close[1], close[0]};
    const EditList r(reversed, options(0, 120));
    CHECK_EQ(r.freezes()[0].area_id, 1);
    CHECK_EQ(r.length(), c.length());
}

void freeze_inside_a_span() {
    // a span from 1.0 s to 3.0 s (frames 60 to 180) with a freeze at frame 120 inside it
    const std::vector<Area> areas = {span(1, 1.0, 3.0, 0.2, 0.2, 0.2, 0.2, "span label"), freeze(2, 120, 60.0, 0.7, 0.7, 0.1, 0.1, "freeze label")};
    const EditList list(areas, options(0, 300));
    CHECK_EQ(list.length(), 300 + 90);
    const Shot base = list.base_shot(120);
    CHECK(base.caption == "span label");
    CHECK(same(base.camera, view_for(areas[0].rect)));
    CHECK(same(list.at(120).shot.camera, base.camera));                // step 0: the span's camera
    CHECK(same(list.at(120 + 90).shot.camera, base.camera));           // back to it
    CHECK(list.at(120 + 10).shot.caption == "span label");             // zooming in: the span's caption
    CHECK(list.at(120 + 45).shot.caption == "freeze label");           // holding: the freeze's
    CHECK(same(list.at(120 + 45).shot.camera, view_for(areas[1].rect)));
    CHECK(list.at(120 + 80).shot.caption == "span label");
    // the span goes on after it with the pair's time
    CHECK_EQ(list.at(120 + 91).pair, 121);
    CHECK(same(list.at(120 + 91).shot.camera, camera_at(camera_keys(areas, 0.5), 121.0 / 60.0)));
    CHECK_EQ(list.at(120 + 91 + 58).pair, 179);
    CHECK(list.at(120 + 91 + 58).shot.caption == "span label");      // the last pair of the span
    CHECK_EQ(list.at(120 + 91 + 59).pair, 180);
    CHECK(list.at(120 + 91 + 59).shot.caption.empty());              // the span's end
}

void the_ends_and_the_outside() {
    // the first frame of the range: the video starts with the sequence
    const EditList start({freeze(1, 100, 60.0, 0.1, 0.1, 0.2, 0.2)}, options(100, 200));
    CHECK_EQ(start.length(), 190);
    CHECK_EQ(start.at(0).pair, 100);
    CHECK_EQ(start.at(0).step, 0);
    CHECK_EQ(start.at(90).pair, 100);
    CHECK_EQ(start.at(91).pair, 101);
    // the last frame: the video ends with it, back at the base camera
    const EditList last({freeze(1, 199, 60.0, 0.1, 0.1, 0.2, 0.2)}, options(100, 200));
    CHECK_EQ(last.length(), 190);
    CHECK_EQ(last.at(98).pair, 198);
    CHECK_EQ(last.at(99).pair, 199);
    CHECK_EQ(last.at(189).pair, 199);
    CHECK_EQ(last.at(189).step, 90);
    CHECK(same(last.at(189).shot.camera, Camera()));
    // outside the range (the overlap, or the exported part of it): left out, with the reason
    const EditList out({freeze(1, 99, 60.0, 0.1, 0.1, 0.2, 0.2), freeze(2, 200, 60.0, 0.1, 0.1, 0.2, 0.2),
                        freeze(3, 150, 60.0, 0.1, 0.1, 0.2, 0.2)},
                       options(100, 200));
    CHECK_EQ(out.length(), 190);
    CHECK_EQ(out.freezes().size(), (size_t)1);
    CHECK_EQ(out.skipped().size(), (size_t)2);
    CHECK_EQ(out.skipped()[0].area_id, 1);
    CHECK(out.skipped()[1].why.find("outside") != std::string::npos);
    // no zoom and no hold: nothing to insert
    EditOptions flat = options(0, 100);
    flat.ease = 0.0;
    flat.freeze_hold = 0.0;
    const EditList nothing({freeze(1, 50, 60.0, 0.1, 0.1, 0.2, 0.2)}, flat);
    CHECK_EQ(nothing.length(), 100);
    CHECK_EQ(nothing.skipped().size(), (size_t)1);
    // an empty range
    CHECK_EQ(EditList({}, options(10, 10)).length(), 0);
    CHECK_EQ(EditList({freeze(1, 10, 60.0, 0.1, 0.1, 0.2, 0.2)}, options(10, 10)).length(), 0);
}

void overrides_and_cuts() {
    // an area's own hold and ease; the zoom time of 0 is a cut
    Area a = freeze(1, 10, 60.0, 0.4, 0.4, 0.1, 0.1);
    a.hold = 1.0;
    a.ease = 0.25;
    const EditList list({a}, options(0, 100));
    CHECK_EQ(list.freezes()[0].ease_frames, 15);
    CHECK_EQ(list.freezes()[0].hold_frames, 60);
    CHECK_EQ(list.length(), 100 + 90);
    Area cut = freeze(1, 10, 60.0, 0.4, 0.4, 0.1, 0.1);
    cut.ease = 0.0;
    const EditList c({cut}, options(0, 100));
    CHECK_EQ(c.length(), 100 + 30);
    CHECK(same(c.at(9).shot.camera, Camera()));
    CHECK(same(c.at(10).shot.camera, view_for(cut.rect)));       // the frame itself is already zoomed
    CHECK(same(c.at(10 + 30).shot.camera, view_for(cut.rect)));
    CHECK(same(c.at(10 + 31).shot.camera, Camera()));
    CHECK_EQ(c.at(10 + 31).pair, 11);
    // clamped to 5 s and 60 s
    Area wild = freeze(1, 10, 60.0, 0.4, 0.4, 0.1, 0.1);
    wild.ease = 99.0;
    wild.hold = 999.0;
    CHECK_EQ(EditList({wild}, options(0, 100)).freezes()[0].ease_frames, 300);
    CHECK_EQ(EditList({wild}, options(0, 100)).freezes()[0].hold_frames, 3600);
}

void dividers() {
    Area sweep = freeze(1, 10, 60.0, 0.4, 0.4, 0.1, 0.1);
    sweep.divider.kind = AreaDivider::SWEEPING;
    sweep.divider.sweeps = 1;
    const EditList list({sweep}, options(0, 100));
    CHECK_NEAR(list.at(10).shot.divider, 0.3, 1e-12);          // the project's before the zoom
    CHECK_NEAR(list.at(10 + 30).shot.divider, 0.5, 1e-9);      // the sweep's start
    CHECK(list.at(10 + 30 + 7).shot.divider > 0.6);            // a quarter: 0.5 + 0.35
    CHECK_NEAR(list.at(10 + 30 + 7 + 8).shot.divider, 0.5, 0.02);
    CHECK(list.at(10 + 30 + 22).shot.divider < 0.4);
    CHECK_NEAR(list.at(10 + 60).shot.divider, 0.5, 1e-9);
    CHECK_NEAR(list.at(10 + 90).shot.divider, 0.3, 1e-12);     // back to the project's
    Area twice = sweep;  // the default: two swings over the hold
    twice.divider.sweeps = DEFAULT_SWEEPS;
    const EditList two({twice}, options(0, 100));
    CHECK(two.at(10 + 30 + 4).shot.divider > 0.6);              // the first swing's quarter
    CHECK_NEAR(two.at(10 + 30 + 15).shot.divider, 0.5, 0.02);   // one swing done
    CHECK(two.at(10 + 30 + 19).shot.divider > 0.6);             // the second swing's quarter
    Area fixed = freeze(1, 10, 60.0, 0.4, 0.4, 0.1, 0.1);
    fixed.divider.kind = AreaDivider::FIXED;
    fixed.divider.fraction = 0.8;
    const EditList f({fixed}, options(0, 100));
    CHECK_NEAR(f.at(10 + 40).shot.divider, 0.8, 1e-12);
    CHECK_NEAR(f.at(10).shot.divider, 0.3, 1e-12);
    CHECK_NEAR(f.at(10 + 90).shot.divider, 0.3, 1e-12);
    // the divider of a span under the freeze is the base
    const std::vector<Area> under = {span(1, 0.0, 5.0, 0.1, 0.1, 0.5, 0.5), freeze(2, 60, 60.0, 0.4, 0.4, 0.1, 0.1)};
    Area fixed_span = under[0];
    fixed_span.divider.kind = AreaDivider::FIXED;
    fixed_span.divider.fraction = 0.7;
    const EditList u({fixed_span, under[1]}, options(0, 200));
    CHECK_NEAR(u.at(60 + 45).shot.divider, 0.7, 1e-12);
}

void frame_of_a_time() {
    // a freeze's t0 is frame / fps; the frame comes back exactly at the usual rates
    for (double fps : {24.0, 30.0, 59.94, 60.0, 119.88}) {
        EditOptions o = options(0, 1000000);
        o.fps = fps;
        for (int64_t n : {0LL, 1LL, 7LL, 1233LL, 5489LL, 99999LL}) {
            Area a = freeze(1, 0, fps, 0.1, 0.1, 0.2, 0.2);
            a.t0 = a.t1 = (double)n / fps;
            CHECK_EQ(make_freeze(a, o).frame, n);
        }
    }
}

}  // namespace

int main() {
    no_marks();
    one_freeze();
    continuity();
    same_frame_and_close_marks();
    freeze_inside_a_span();
    the_ends_and_the_outside();
    overrides_and_cuts();
    dividers();
    frame_of_a_time();
    return check_result("edit_list");
}
