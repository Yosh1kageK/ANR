// Freeze marks in the player: the clock stops at the mark, the sequence runs in real time with the shots of the edit
// list, playing goes on by itself, and scrubbing, pausing, seeking and the option behave as documented (mark_runner.h).
#include "renderer/clips/mark_runner.h"

#include <cmath>
#include <vector>

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

class AllReady : public Availability {
public:
    bool Ready(int, int64_t) const override { return true; }
};

Area freeze(int id, int64_t frame, double x, double y, double w, double h) {
    Area a;
    a.id = id;
    a.label = "mark " + std::to_string(id);
    a.kind = AreaKind::FREEZE;
    a.kind_stored = true;
    a.t0 = a.t1 = (double)frame / 60.0;
    a.rect[0] = x;
    a.rect[1] = y;
    a.rect[2] = w;
    a.rect[3] = h;
    return a;
}

EditOptions options(int64_t end = 400) {
    EditOptions o;
    o.fps = 60.0;
    o.first = 0;
    o.end = end;
    o.ease = 0.1;          // 6 frames
    o.freeze_hold = 0.1;   // 6 frames: a sequence of 18 inserted frames
    o.divider = 0.5;
    return o;
}

Timeline timeline(int64_t frames = 400) {
    return Timeline(frames, frames, 0, 60.0);
}

// The viewer's loop in virtual time: the runner around the clock's tick, at `hz`.
struct Sim {
    Lockstep clock;
    MarkRunner runner;
    AllReady ready;
    EditList marks;
    double now = 0.0, hz;
    struct Tick {
        int64_t shown;
        MarkRunner::State state;
    };
    std::vector<Tick> ticks;

    Sim(const std::vector<Area>& areas, double display_hz = 120.0, int64_t frames = 400)
        : marks(areas, options(frames)), hz(display_hz) {
        clock.SetTimeline(timeline(frames));
    }
    void Run(double seconds) {
        const int n = (int)std::lround(seconds * hz);
        for (int i = 0; i < n; i++, now += 1.0 / hz) {
            runner.BeforeTick(clock, marks);
            clock.Tick(now, ready);
            const Shot live = marks.base_shot(clock.shown() == NO_PAIR ? 0 : clock.shown());
            ticks.push_back({clock.shown(), runner.AfterTick(now, clock, marks, live)});
        }
    }
    int ActiveTicks(int area = 0) const {
        int n = 0;
        for (const Tick& t : ticks)
            n += t.state.active && (area == 0 || t.state.area_id == area);
        return n;
    }
};

void two_marks_play_through() {
    const std::vector<Area> areas = {freeze(1, 100, 0.4, 0.4, 0.1, 0.1), freeze(2, 200, 0.1, 0.1, 0.2, 0.2)};
    Sim sim(areas);
    sim.clock.Seek(90);
    sim.clock.Play();
    sim.Run(6.0);
    CHECK(!sim.clock.playing() && sim.clock.ended());  // it played to the end by itself
    CHECK_EQ(sim.clock.shown(), 399);
    // each sequence is 1 + 18 frame times on a 120 Hz display (2 ticks per frame time), within a tick or two
    CHECK(std::abs(sim.ActiveTicks(1) - 2 * 19) <= 3);
    CHECK(std::abs(sim.ActiveTicks(2) - 2 * 19) <= 3);
    // the shown pair never moved during a sequence, never skipped or went back otherwise
    int64_t last = -1;
    for (const Sim::Tick& t : sim.ticks) {
        if (t.state.active)
            CHECK_EQ(t.shown, sim.marks.freezes()[t.state.area_id - 1].frame);
        CHECK(t.shown == last || t.shown == last + 1 || last == -1);
        last = t.shown;
    }
    // the steps go 0..18 in order, and each shot is the edit list's at that step
    int expect = 0;
    for (const Sim::Tick& t : sim.ticks) {
        if (!t.state.active || t.state.area_id != 1)
            continue;
        CHECK(t.state.step == expect || t.state.step == expect + 1);
        expect = (int)t.state.step;
        const EditEntry e = sim.marks.at(sim.marks.first_output_of(100) + t.state.step);
        CHECK(e.pair == 100 && e.step == t.state.step);
        CHECK(e.shot.camera == t.state.shot.camera);
        CHECK(e.shot.caption == t.state.shot.caption);
        CHECK_NEAR(e.shot.divider, t.state.shot.divider, 1e-12);
    }
    CHECK_EQ(expect, 18);
    // phases in order
    FreezePhase phase = FreezePhase::ZOOMING_IN;
    for (const Sim::Tick& t : sim.ticks)
        if (t.state.active && t.state.area_id == 1) {
            CHECK((int)t.state.phase >= (int)phase);
            phase = t.state.phase;
        }
    CHECK(phase == FreezePhase::ZOOMING_OUT);
    // the whole run took (400 - 90 + 36 frames) of 60 fps: 5.77 s, give or take a frame
    int64_t first_tick_at_end = -1;
    for (size_t i = 0; i < sim.ticks.size(); i++)
        if (sim.ticks[i].shown == 399) {
            first_tick_at_end = (int64_t)i;
            break;
        }
    CHECK(std::fabs(first_tick_at_end / 120.0 - (309 + 36) / 60.0) < 0.1);
}

void slow_displays_miss_nothing() {
    const std::vector<Area> areas = {freeze(1, 100, 0.4, 0.4, 0.1, 0.1), freeze(2, 101, 0.1, 0.1, 0.2, 0.2)};
    for (double hz : {60.0, 30.0, 15.0}) {
        Sim sim(areas, hz);
        sim.clock.Seek(90);
        sim.clock.Play();
        sim.Run(4.0);
        CHECK(sim.ActiveTicks(1) > 0 && sim.ActiveTicks(2) > 0);
        // the pair 100 was on screen for its sequence, and the pair 101 for the second one
        for (const Sim::Tick& t : sim.ticks)
            if (t.state.active)
                CHECK_EQ(t.shown, t.state.area_id == 1 ? 100 : 101);
        CHECK(sim.clock.shown() > 101);
    }
}

void pause_seek_and_the_option() {
    const std::vector<Area> areas = {freeze(1, 100, 0.4, 0.4, 0.1, 0.1)};
    // pausing during the sequence pauses it; playing goes on
    {
        Sim sim(areas);
        sim.clock.Seek(90);
        sim.clock.Play();
        sim.Run(0.3);  // the pair 100 came up at 0.17 s and the sequence is under way
        CHECK(sim.ticks.back().state.active);
        const int64_t step = sim.ticks.back().state.step;
        CHECK(step > 5 && step < 18);
        sim.clock.Pause();
        sim.Run(2.0);
        CHECK(sim.ticks.back().state.active && sim.ticks.back().state.step == step);  // held
        CHECK_EQ(sim.clock.position(), 100);
        sim.clock.Play();
        sim.Run(1.0);
        CHECK(!sim.ticks.back().state.active);
        CHECK(sim.clock.shown() > 100);  // on from the frame after the mark
    }
    // a seek during the sequence ends it; playing again from before the mark runs it again
    {
        Sim sim(areas);
        sim.clock.Seek(90);
        sim.clock.Play();
        sim.Run(0.3);
        CHECK(sim.ticks.back().state.active);
        sim.clock.Seek(50);
        sim.Run(0.1);
        CHECK(!sim.ticks.back().state.active);
        const int before = sim.ActiveTicks();
        sim.Run(1.5);  // 50 to 140: through the mark again
        CHECK(sim.ActiveTicks() > before + 30);
    }
    // the option off: no stop, no sequence; pairs flow at the frame rate
    {
        Sim sim(areas);
        sim.runner.SetEnabled(false);
        sim.clock.Seek(90);
        sim.clock.Play();
        sim.Run(1.0);
        CHECK_EQ(sim.ActiveTicks(), 0);
        CHECK(sim.clock.shown() >= 148 && sim.clock.shown() <= 150);
        CHECK_EQ(sim.clock.stop(), NO_PAIR);
    }
    // turned off during a sequence: it ends
    {
        Sim sim(areas);
        sim.clock.Seek(90);
        sim.clock.Play();
        sim.Run(0.3);
        CHECK(sim.ticks.back().state.active);
        sim.runner.SetEnabled(false);
        sim.Run(0.1);
        CHECK(!sim.ticks.back().state.active);
        sim.Run(0.5);
        CHECK(sim.clock.shown() > 101);
    }
    // paused viewing and stepping do not run marks
    {
        Sim sim(areas);
        sim.clock.Seek(99);
        sim.Run(0.5);
        CHECK_EQ(sim.ActiveTicks(), 0);
        sim.clock.Step(1);
        sim.Run(0.5);
        CHECK_EQ(sim.clock.shown(), 100);
        sim.clock.Step(1);
        sim.Run(0.5);
        CHECK_EQ(sim.ActiveTicks(), 0);
        CHECK_EQ(sim.clock.stop(), NO_PAIR);
    }
    // a deleted mark ends its sequence
    {
        Sim sim(areas);
        sim.clock.Seek(90);
        sim.clock.Play();
        sim.Run(0.3);
        CHECK(sim.ticks.back().state.active);
        sim.marks = EditList({}, options());
        sim.Run(0.2);
        CHECK(!sim.ticks.back().state.active);
        sim.Run(0.5);
        CHECK(sim.clock.shown() > 101);
    }
    // time runs at the clock's speed
    {
        Sim sim(areas);
        sim.clock.SetSpeed(0.5);
        sim.clock.Seek(98);
        sim.clock.Play();
        sim.Run(2.5);
        CHECK(std::abs(sim.ActiveTicks() - 4 * 19) <= 4);  // twice as long at half speed
    }
}

void playing_from_a_marked_frame() {
    const std::vector<Area> areas = {freeze(1, 100, 0.4, 0.4, 0.1, 0.1)};
    Sim sim(areas);
    sim.clock.Seek(100);
    sim.Run(0.2);  // paused on the frame: nothing runs
    CHECK_EQ(sim.ActiveTicks(), 0);
    sim.clock.Play();
    sim.Run(1.0);
    CHECK(std::abs(sim.ActiveTicks() - 2 * 19) <= 3);  // the mark of the frame runs
    CHECK(sim.clock.shown() > 100);
    // not again while the clock is still at the frame: pause on the tick the sequence ended on, then play
    Sim again(areas);
    again.clock.Seek(98);
    again.clock.Play();
    bool was_active = false;
    for (int i = 0; i < 1000; i++) {
        again.Run(1.0 / again.hz);
        if (again.ticks.back().state.active)
            was_active = true;
        else if (was_active)
            break;
    }
    CHECK(was_active && !again.ticks.back().state.active);
    again.clock.Pause();
    const int ran = again.ActiveTicks();
    again.Run(0.5);
    CHECK_EQ(again.ActiveTicks(), ran);
    CHECK_EQ(again.clock.position(), 100);
    again.clock.Play();
    again.Run(1.0);
    CHECK_EQ(again.ActiveTicks(), ran);  // it plays on
    CHECK(again.clock.shown() > 100);
}

void marks_at_one_frame_and_at_the_end() {
    const std::vector<Area> same = {freeze(1, 100, 0.4, 0.4, 0.1, 0.1), freeze(2, 100, 0.1, 0.1, 0.2, 0.2)};
    Sim sim(same);
    sim.clock.Seek(98);
    sim.clock.Play();
    sim.Run(2.0);
    CHECK_EQ(sim.marks.length(), 400 + 36);
    // one after the other: 1 + 18 + 18 frame times in all for the frame, the second one from step 1
    CHECK(std::abs(sim.ActiveTicks() - 2 * 37) <= 5);
    int64_t second_first_step = -1;
    for (const Sim::Tick& t : sim.ticks)
        if (t.state.active && t.state.area_id == 2) {
            second_first_step = t.state.step;
            break;
        }
    CHECK_EQ(second_first_step, 1);
    for (const Sim::Tick& t : sim.ticks)
        if (t.state.active)
            CHECK_EQ(t.shown, 100);
    // a mark on the last frame: the video ends with it
    const std::vector<Area> last = {freeze(1, 399, 0.4, 0.4, 0.1, 0.1)};
    Sim end(last);
    end.clock.Seek(390);
    end.clock.Play();
    end.Run(2.0);
    CHECK(end.ActiveTicks() > 30);
    CHECK(!end.clock.playing() && end.clock.ended());
    CHECK_EQ(end.clock.shown(), 399);
    CHECK(!end.ticks.back().state.active);
    // a mark on the first frame runs when playing starts there
    const std::vector<Area> first = {freeze(1, 0, 0.4, 0.4, 0.1, 0.1)};
    Sim start(first);
    start.clock.Play();
    start.Run(1.0);
    CHECK(std::abs(start.ActiveTicks() - 2 * 19) <= 3);
    CHECK(start.clock.shown() > 5);
}

void preview() {
    const std::vector<Area> areas = {freeze(1, 100, 0.4, 0.4, 0.1, 0.1)};
    Sim sim(areas);
    sim.runner.SetEnabled(false);  // a preview runs whatever the option says
    sim.runner.StartPreview(sim.marks.freezes()[0], sim.clock);
    CHECK(!sim.clock.playing());
    sim.Run(1.0);
    CHECK(std::abs(sim.ActiveTicks() - 2 * 19) <= 3);
    CHECK(sim.ticks.front().state.preview || sim.ticks[5].state.preview);
    CHECK(!sim.clock.playing());           // paused after it
    CHECK_EQ(sim.clock.position(), 100);
    CHECK(!sim.ticks.back().state.active);
    // again: a preview does not count as run
    sim.runner.StartPreview(sim.marks.freezes()[0], sim.clock);
    const int before = sim.ActiveTicks();
    sim.Run(1.0);
    CHECK(sim.ActiveTicks() > before + 30);
    // no zoom and no hold: nothing to preview
    Area flat = freeze(2, 50, 0.1, 0.1, 0.2, 0.2);
    flat.ease = 0.0;
    flat.hold = 0.0;
    MarkRunner runner;
    Lockstep clock;
    clock.SetTimeline(timeline());
    runner.StartPreview(make_freeze(flat, options()), clock);
    CHECK(!runner.running());
}

}  // namespace

int main() {
    two_marks_play_through();
    slow_displays_miss_nothing();
    pause_seek_and_the_option();
    playing_from_a_marked_frame();
    marks_at_one_frame_and_at_the_end();
    preview();
    return check_result("mark_runner");
}
