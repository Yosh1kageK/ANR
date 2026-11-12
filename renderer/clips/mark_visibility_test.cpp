// When a mark's rectangle shows: the ramp in and out around a span and around a freeze's sequence, the settings, and that it is
// continuous (no pop) and never leaves 0 to 1.
#include "renderer/clips/mark_visibility.h"

#include <cmath>

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

double span(double playhead, double lead = 1.0, double trail = 1.0) {
    return mark_alpha(playhead, AreaKind::SPAN, 10.0, 14.0, 0.0, lead, trail);  // a span of 10 s to 14 s
}

double freeze(double playhead, double lead = 1.0, double trail = 1.0) {
    return mark_alpha(playhead, AreaKind::FREEZE, 20.0, 20.0, 2.5, lead, trail);  // a frame at 20 s, a sequence of 2.5 s
}

void a_span_fades_in_before_t0_and_out_after_t1() {
    CHECK_NEAR(span(5.0), 0.0, 1e-12);       // far before
    CHECK_NEAR(span(8.99), 0.0, 1e-12);      // just outside the lead
    CHECK_NEAR(span(9.0), 0.0, 1e-12);       // the lead begins at 0
    CHECK_NEAR(span(9.5), 0.5, 1e-12);       // the smoothstep is 0.5 half way
    CHECK_NEAR(span(10.0), 1.0, 1e-12);      // whole from t0
    CHECK_NEAR(span(12.0), 1.0, 1e-12);      // inside
    CHECK_NEAR(span(14.0), 1.0, 1e-12);      // to t1
    CHECK_NEAR(span(14.5), 0.5, 1e-12);      // the trail half way
    CHECK_NEAR(span(15.0), 0.0, 1e-12);      // gone after the trail
    CHECK_NEAR(span(40.0), 0.0, 1e-12);
    // a quarter of the way is a smoothstep, not a line
    CHECK_NEAR(span(9.25), 0.15625, 1e-12);  // smoothstep(0.25)
    CHECK_NEAR(span(14.75), 0.15625, 1e-12);
}

void a_freeze_lasts_to_the_end_of_its_sequence() {
    CHECK_NEAR(freeze(18.0), 0.0, 1e-12);
    CHECK_NEAR(freeze(19.5), 0.5, 1e-12);    // the lead before the frame
    CHECK_NEAR(freeze(20.0), 1.0, 1e-12);    // the frame itself (the playhead stands there during the sequence)
    CHECK_NEAR(freeze(21.0), 1.0, 1e-12);
    CHECK_NEAR(freeze(22.5), 1.0, 1e-12);    // the end of the sequence, zoom out included
    CHECK_NEAR(freeze(23.0), 0.5, 1e-12);    // half way down the trail
    CHECK_NEAR(freeze(23.5), 0.0, 1e-12);
    // a sequence of no length (no zoom and no hold): the frame's time only
    CHECK_NEAR(mark_alpha(20.0, AreaKind::FREEZE, 20.0, 20.0, 0.0, 1.0, 1.0), 1.0, 1e-12);
    CHECK_NEAR(mark_alpha(20.5, AreaKind::FREEZE, 20.0, 20.0, 0.0, 1.0, 1.0), 0.5, 1e-12);
    // a freeze's t1 (equal to t0) and a negative length change nothing
    CHECK_NEAR(mark_alpha(21.0, AreaKind::FREEZE, 20.0, 20.0, -3.0, 1.0, 1.0), 0.0, 1e-12);
}

void lead_and_trail_are_settings() {
    CHECK_NEAR(span(9.0, 2.0), 0.5, 1e-12);                                      // 2 s of lead: 9.0 is half way
    CHECK_NEAR(span(8.0, 2.0), 0.0, 1e-12);
    CHECK_NEAR(span(16.0, 1.0, 4.0), 0.5, 1e-12);                               // 4 s of trail: 16.0 is half way
    CHECK_NEAR(span(19.0, 1.0, 4.0), 0.0, 1e-12);
    // 0 is a cut: whole inside, nothing outside
    CHECK_NEAR(span(9.99, 0.0, 0.0), 0.0, 1e-12);
    CHECK_NEAR(span(10.0, 0.0, 0.0), 1.0, 1e-12);
    CHECK_NEAR(span(14.0, 0.0, 0.0), 1.0, 1e-12);
    CHECK_NEAR(span(14.01, 0.0, 0.0), 0.0, 1e-12);
    // an empty span (t1 before t0) is its t0
    CHECK_NEAR(mark_alpha(10.0, AreaKind::SPAN, 10.0, 8.0, 0.0, 1.0, 1.0), 1.0, 1e-12);
    CHECK_NEAR(mark_alpha(10.5, AreaKind::SPAN, 10.0, 8.0, 0.0, 1.0, 1.0), 0.5, 1e-12);
}

void the_setting_decides() {
    MarkShowSettings s;
    CHECK(s.show == MarkShow::NEAR_PLAYHEAD);  // the default
    CHECK_NEAR(s.lead, 1.0, 1e-12);
    CHECK_NEAR(s.trail, 1.0, 1e-12);
    CHECK_NEAR(mark_alpha(s, 30.0, AreaKind::SPAN, 10.0, 14.0, 0.0), 0.0, 1e-12);
    CHECK_NEAR(mark_alpha(s, 12.0, AreaKind::SPAN, 10.0, 14.0, 0.0), 1.0, 1e-12);
    s.show = MarkShow::ALWAYS;
    CHECK_NEAR(mark_alpha(s, 30.0, AreaKind::SPAN, 10.0, 14.0, 0.0), 1.0, 1e-12);
    CHECK_NEAR(mark_alpha(s, 30.0, AreaKind::FREEZE, 10.0, 10.0, 1.0), 1.0, 1e-12);
    s.show = MarkShow::NEVER;
    CHECK_NEAR(mark_alpha(s, 12.0, AreaKind::SPAN, 10.0, 14.0, 0.0), 0.0, 1e-12);
    s.show = MarkShow::NEAR_PLAYHEAD;
    s.lead = 3.0;
    s.trail = 0.5;
    CHECK_NEAR(mark_alpha(s, 8.5, AreaKind::SPAN, 10.0, 14.0, 0.0), smoothstep(1.0 - 1.5 / 3.0), 1e-12);
    CHECK_NEAR(mark_alpha(s, 14.25, AreaKind::SPAN, 10.0, 14.0, 0.0), 0.5, 1e-12);
}

// A playhead that moves a frame at a time never makes the rectangle jump: the step between two frames of a 60 fps
// timeline is small, rises to 1 and falls to 0 once, and stays within 0 and 1.
void the_ramp_has_no_pop() {
    for (AreaKind kind : {AreaKind::SPAN, AreaKind::FREEZE}) {
        double previous = 0.0, largest_step = 0.0, peak = 0.0;
        int rises = 0, falls = 0;
        for (int frame = 0; frame <= 60 * 40; frame++) {
            const double a = mark_alpha(frame / 60.0, kind, 10.0, 14.0, 3.0, 1.0, 1.0);
            CHECK(a >= 0.0 && a <= 1.0);
            largest_step = std::max(largest_step, std::fabs(a - previous));
            if (a > previous + 1e-12 && previous < 1.0)
                rises++;
            if (a < previous - 1e-12)
                falls++;
            peak = std::max(peak, a);
            previous = a;
        }
        CHECK_NEAR(peak, 1.0, 1e-12);
        CHECK(largest_step < 0.04);     // 1 s of lead is 60 frames: a smoothstep climbs at most 1.5 / 60 per frame
        CHECK(rises > 0 && falls > 0);
    }
}

}  // namespace

int main() {
    a_span_fades_in_before_t0_and_out_after_t1();
    a_freeze_lasts_to_the_end_of_its_sequence();
    lead_and_trail_are_settings();
    the_setting_decides();
    the_ramp_has_no_pop();
    return check_result("mark_visibility");
}
