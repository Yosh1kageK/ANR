// The clock and the pairing, against simulated decoders in virtual time: healthy, slow (decode-limited), stalling,
// seeking, stepping, with frames that exist in only one recording, an offset change and the end of the timeline.
#include "renderer/clips/lockstep.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "renderer/clips/check.h"
#include "renderer/clips/edit_list.h"

using namespace renderer::clips;

namespace {

// Two decoders that produce frames in order at `rate` frames per second of virtual time, at most `ahead` frames beyond
// the clock's frame (they idle when their ring is full), with windows in which one of them produces nothing. Frames
// before `first` are gone (a restart after a seek).
class SimDecoders : public Availability {
public:
    // position: where the clock starts; each decoder starts at its recording's frame there (as the real ones restart at
    // the frame they need).
    SimDecoders(const Timeline& t, double rate_a, double rate_b, int64_t position = -1, int64_t ahead = 45)
        : timeline_(t), ahead_(ahead) {
        rate_[0] = rate_a;
        rate_[1] = rate_b;
        if (position == -1)
            position = t.has_overlap() ? t.overlap_first() : t.first();
        for (int v = 0; v < 2; v++) {
            const int64_t frames = v == 0 ? t.frames_a : t.frames_b;
            first_[v] = next_[v] = std::clamp<int64_t>(v == 0 ? position : t.b_frame(position), 0, std::max<int64_t>(frames - 1, 0));
        }
    }

    struct Stall {
        int video;
        double t0, t1;
    };
    std::vector<Stall> stalls;

    // The decoders see the clock's position (frames of A) and move on by dt.
    void Step(double now, double dt, int64_t position) {
        for (int v = 0; v < 2; v++) {
            const int64_t frames = v == 0 ? timeline_.frames_a : timeline_.frames_b;
            const int64_t head = std::clamp<int64_t>(v == 0 ? position : timeline_.b_frame(position), 0, std::max<int64_t>(frames - 1, 0));
            bool stalled = false;
            for (const Stall& s : stalls)
                stalled |= s.video == v && now >= s.t0 && now < s.t1;
            if (stalled || now < start_[v])
                continue;
            credit_[v] += rate_[v] * dt;
            const int64_t limit = std::min(frames, head + ahead_ + 1);
            while (credit_[v] >= 1.0 && next_[v] < limit) {
                next_[v]++;
                credit_[v] -= 1.0;
                decoded_[v]++;
            }
            if (next_[v] >= limit)
                credit_[v] = 0.0;
        }
    }

    // Everything of video v from `frame` on is decoded again (a seek): the frames before are gone, the decoder starts
    // after `latency` seconds.
    void Restart(int v, int64_t frame, double now, double latency) {
        first_[v] = frame;
        next_[v] = frame;
        start_[v] = now + latency;
        credit_[v] = 0.0;
    }

    // Decoded pairs per second the decoders sustain, as a source would report it (frames per busy second).
    double Capability() const { return std::min(rate_[0], rate_[1]); }
    void SetRate(int v, double rate) { rate_[v] = rate; }

    bool Ready(int video, int64_t frame) const override {
        return frame >= first_[video] && frame < next_[video];
    }
    int64_t decoded(int v) const { return decoded_[v]; }

private:
    Timeline timeline_;
    int64_t ahead_;
    double rate_[2];
    double start_[2] = {0.0, 0.0};
    double credit_[2] = {0.0, 0.0};
    int64_t first_[2], next_[2];
    int64_t decoded_[2] = {0, 0};
};

struct Run {
    std::vector<int64_t> shown;  // pairs in the order they were put on screen
    int64_t jumps_over_one = 0;
    int64_t unready = 0;         // a new pair whose frames were not decoded (must stay 0)
    int ticks_buffering = 0;
    double buffering_seconds = 0.0;
};

// Ticks the clock at `hz` for `seconds` of virtual time.
Run play(Lockstep& clock, SimDecoders& sim, double hz, double seconds, double start = 0.0, double* now_out = nullptr,
         bool report_capability = true) {
    Run run;
    double now = start;
    const double dt = 1.0 / hz;
    for (int i = 0; i < (int)std::lround(seconds * hz); i++, now += dt) {
        sim.Step(now, dt, clock.position());
        const Lockstep::Decision d = clock.Tick(now, sim, report_capability ? sim.Capability() : 0.0);
        if (d.changed) {
            if ((d.has_a && !sim.Ready(0, d.pair)) || (d.has_b && !sim.Ready(1, d.b_frame)))
                run.unready++;
            if (!run.shown.empty() && d.pair > run.shown.back() + 1)
                run.jumps_over_one++;
            run.shown.push_back(d.pair);
        }
        if (d.buffering) {
            run.ticks_buffering++;
            run.buffering_seconds += dt;
        }
    }
    if (now_out)
        *now_out = now;
    return run;
}

Timeline sample_timeline() {
    return Timeline(5490, 5076, 413, 60.0);
}

void timeline_geometry() {
    const Timeline t = sample_timeline();
    CHECK_EQ(t.first(), 0);
    CHECK_EQ(t.end(), 5490);
    CHECK_EQ(t.overlap_first(), 413);
    CHECK_EQ(t.overlap_end(), 5489);
    CHECK(t.has_a(0) && !t.has_b(0) && !t.has_b(412) && t.has_b(413) && t.has_b(5488) && !t.has_b(5489));
    const Timeline early(t.frames_a, t.frames_b, -100, t.fps);  // B started first
    CHECK_EQ(early.first(), -100);
    CHECK_EQ(early.end(), std::max<int64_t>(5490, 5076 - 100));
    CHECK(!early.has_a(-1) && early.has_b(-100) && early.has_a(0));
}

void healthy_decoders() {
    Timeline t = sample_timeline();
    Lockstep clock;
    clock.SetTimeline(t);
    SimDecoders sim(t, 240.0, 240.0);
    clock.Play();
    // let the decoders get ahead of the start (a real start waits for the first pair)
    Run run = play(clock, sim, 120.0, 10.0);
    CHECK_EQ(run.unready, 0);
    CHECK_EQ(run.jumps_over_one, 0);
    // 120 Hz ticks of a 60 fps recording: every pair once. The first pair is at the start of the overlap.
    CHECK_EQ(run.shown.front(), 413);
    CHECK(std::llabs((int64_t)run.shown.size() - 600) <= 2);
    CHECK_EQ(clock.stats().pairs_held, 0u);
    CHECK_EQ(clock.stats().stalls, 0u);
    CHECK_NEAR(clock.stats().rate_min, 1.0, 1e-9);
    for (size_t i = 1; i < run.shown.size(); i++)
        CHECK_EQ(run.shown[i], run.shown[i - 1] + 1);
    // a display slower than the recording (30 Hz for 60 fps): every other pair, none mismatched, none unready
    Lockstep slow_display;
    slow_display.SetTimeline(t);
    SimDecoders sim2(t, 240.0, 240.0);
    slow_display.Play();
    Run run2 = play(slow_display, sim2, 30.0, 10.0);
    CHECK_EQ(run2.unready, 0);
    CHECK(std::llabs((int64_t)run2.shown.size() - 300) <= 2);
    CHECK(run2.jumps_over_one > 250);  // frames are skipped, the pairs stay pairs
}

void decode_limited() {
    Timeline t = sample_timeline();
    Lockstep clock;
    clock.SetTimeline(t);
    // 30 pairs per second against 60 fps: half real time
    SimDecoders sim(t, 30.0, 40.0);
    clock.Play();
    double now = 0.0;
    Run run = play(clock, sim, 120.0, 20.0, 0.0, &now);
    CHECK_EQ(run.unready, 0);
    CHECK(clock.decode_limited());
    CHECK(clock.rate() < 0.5 && clock.rate() >= 0.4);  // 30 / 60 times the margin, in steps of 0.05
    // after the rate settled there are no more stalls: the last 10 s show a steady rate
    const uint64_t stalls = clock.stats().stalls;
    Run later = play(clock, sim, 120.0, 10.0, now, &now);
    CHECK_EQ(clock.stats().stalls, stalls);
    CHECK(std::fabs((double)later.shown.size() / 10.0 - clock.rate() * 60.0) < 3.0);
    CHECK_EQ(later.jumps_over_one, 0);
    CHECK(clock.stats().rate_min < 0.5);
    // the decoders recover: the rate climbs back to the requested one, step by step
    sim.SetRate(0, 240.0);
    sim.SetRate(1, 240.0);
    play(clock, sim, 120.0, 15.0, now, &now);
    CHECK_NEAR(clock.rate(), 1.0, 1e-9);
    CHECK(!clock.decode_limited());
}

void stalling_decoder() {
    Timeline t = sample_timeline();
    Lockstep clock;
    clock.SetTimeline(t);
    SimDecoders sim(t, 150.0, 150.0);
    sim.stalls.push_back({1, 4.0, 5.5});  // B stops for a second and a half
    clock.Play();
    double now = 0.0;
    Run run = play(clock, sim, 120.0, 12.0, 0.0, &now, /*report_capability=*/false);
    CHECK_EQ(run.unready, 0);
    // the pair was held while buffering and playback went on from where it stopped: no pair after the stall is more
    // than the next one (nothing is caught up), and none was shown twice or out of order
    CHECK_EQ(run.jumps_over_one, 0);
    CHECK(run.buffering_seconds > 0.5);
    CHECK_EQ(clock.stats().stalls, 1u);
    CHECK(clock.stats().pairs_held > 50);
    for (size_t i = 1; i < run.shown.size(); i++)
        CHECK_EQ(run.shown[i], run.shown[i - 1] + 1);
    // only the decoded part is shown: the decode-ahead ring (45 frames) lasts 0.75 s of the 1.5 s stall
    CHECK(run.buffering_seconds < 1.5);
    // the stall lowered the rate a little (it climbs back with calm)
    CHECK(clock.stats().rate_min < 1.0);
    CHECK(clock.stats().rate_min >= 0.8);
}

void pairs_never_mismatch_across_long_play() {
    // A full playthrough of the sample's length at a rate decoders only just sustain, with random stalls
    Timeline t = sample_timeline();
    Lockstep clock;
    clock.SetTimeline(t);
    SimDecoders sim(t, 75.0, 70.0);
    for (int i = 0; i < 12; i++)
        sim.stalls.push_back({i % 2, 5.0 + 6.0 * i, 5.0 + 6.0 * i + 0.4 + 0.1 * (i % 5)});
    clock.Play();
    const Run run = play(clock, sim, 120.0, 100.0, 0.0, nullptr, true);
    CHECK_EQ(run.unready, 0);
    CHECK_EQ(run.jumps_over_one, 0);
    for (size_t i = 1; i < run.shown.size(); i++)
        CHECK(run.shown[i] > run.shown[i - 1]);
    CHECK(run.shown.size() > 3000);
    CHECK(clock.stats().rate_min <= 1.0);
}

void seeks_and_steps() {
    Timeline t = sample_timeline();
    Lockstep clock;
    clock.SetTimeline(t);
    SimDecoders sim(t, 240.0, 240.0);
    clock.Play();
    double now = 0.0;
    play(clock, sim, 120.0, 3.0, 0.0, &now);
    clock.Pause();
    const int64_t at = clock.shown();
    // a step back into the decoded frames is shown by the next tick, and nothing else happens
    clock.Step(-1);
    Lockstep::Decision d = clock.Tick(now, sim);
    CHECK(d.changed);
    CHECK_EQ(d.pair, at - 1);
    CHECK(!d.buffering);
    d = clock.Tick(now + 0.01, sim);
    CHECK(!d.changed);
    CHECK_EQ(d.pair, at - 1);
    // a forward step shows the next frame
    clock.Step(2);
    d = clock.Tick(now + 0.02, sim);
    CHECK_EQ(d.pair, at + 1);
    // a seek far away: held, buffering, until both frames are decoded
    clock.Seek(3000);
    sim.Restart(0, 3000, now, 0.3);
    sim.Restart(1, t.b_frame(3000), now, 0.3);
    d = clock.Tick(now + 0.03, sim);
    CHECK(d.buffering);
    CHECK(!d.changed);
    CHECK_EQ(d.pair, at + 1);  // the old pair stays up
    bool shown = false;
    for (int i = 0; i < 200 && !shown; i++) {
        const double at_time = now + 0.04 + i * 0.01;
        sim.Step(at_time, 0.01, 3000);
        d = clock.Tick(at_time, sim);
        shown = d.changed;
        if (shown)
            CHECK_EQ(d.pair, 3000);
    }
    CHECK(shown);
    // seeking while playing waits too, without counting a stall, and then plays on from the seek target
    Lockstep playing;
    playing.SetTimeline(t);
    SimDecoders sim2(t, 240.0, 240.0);
    playing.Play();
    double n2 = 0.0;
    play(playing, sim2, 120.0, 2.0, 0.0, &n2);
    playing.Seek(1500);
    sim2.Restart(0, 1500, n2, 0.4);
    sim2.Restart(1, t.b_frame(1500), n2, 0.4);
    const uint64_t stalls = playing.stats().stalls;
    const Run after = play(playing, sim2, 120.0, 3.0, n2, &n2);
    CHECK_EQ(after.shown.front(), 1500);
    CHECK_EQ(playing.stats().stalls, stalls);
    CHECK_EQ(after.unready, 0);
    CHECK_EQ(after.jumps_over_one, 0);
}

void one_recording_only() {
    Timeline t = sample_timeline();
    Lockstep clock;
    clock.SetTimeline(t);
    // B is not decoded at all (it does not exist yet): the A-only frames before the overlap are shown on time
    SimDecoders sim(t, 240.0, 0.0, 0);
    clock.Seek(0);
    clock.Play();
    int a_only = 0;
    double now = 0.0;
    for (int i = 0; i < 240; i++, now += 1.0 / 120.0) {
        sim.Step(now, 1.0 / 120.0, clock.position());
        const Lockstep::Decision d = clock.Tick(now, sim);
        if (d.changed) {
            CHECK(d.has_a);
            CHECK(!d.has_b);
            CHECK_EQ(d.b_frame, -1);
            a_only++;
        }
    }
    CHECK(a_only > 100);  // two seconds of A-only frames, once each
    // a recording that started first: B-only frames, labelled
    const Timeline early(t.frames_a, t.frames_b, -120, t.fps);
    Lockstep other;
    other.SetTimeline(early);
    CHECK_EQ(other.position(), 0);  // starts at the overlap
    other.Seek(-120);
    SimDecoders sim3(early, 0.0, 240.0, -120);
    other.Play();
    now = 0.0;
    int b_only = 0;
    for (int i = 0; i < 240; i++, now += 1.0 / 120.0) {
        sim3.Step(now, 1.0 / 120.0, other.position());
        const Lockstep::Decision d = other.Tick(now, sim3);
        if (d.changed && d.pair < 0) {
            CHECK(!d.has_a);
            CHECK(d.has_b);
            CHECK_EQ(d.b_frame, d.pair + 120);
            b_only++;
        }
    }
    CHECK(b_only > 50);
}

void offset_change() {
    Timeline t = sample_timeline();
    Lockstep clock;
    clock.SetTimeline(t);
    SimDecoders sim(t, 240.0, 240.0);
    clock.Seek(2000);
    sim.Restart(0, 2000 - 60, 0.0, 0.0);
    sim.Restart(1, t.b_frame(2000) - 60, 0.0, 0.0);
    sim.Step(0.0, 1.0, 2000);
    Lockstep::Decision d = clock.Tick(0.0, sim);
    CHECK(d.changed);
    CHECK_EQ(d.b_frame, 2000 - 413);
    // the offset moves by one: the pair at the same position now has B's next frame, once it is decoded
    clock.SetOffset(414);
    d = clock.Tick(0.1, sim);
    CHECK(d.changed);
    CHECK_EQ(d.pair, 2000);
    CHECK_EQ(d.b_frame, 2000 - 414);
    // B's frame is not decoded: the old pair stays up
    clock.SetOffset(500);
    d = clock.Tick(0.2, sim);
    CHECK(d.buffering);
    CHECK(!d.changed);
}

void end_of_timeline() {
    const Timeline t(300, 200, 20, 60.0);
    Lockstep clock;
    clock.SetTimeline(t);
    SimDecoders sim(t, 500.0, 500.0);
    clock.Play();
    const Run run = play(clock, sim, 120.0, 10.0);
    CHECK_EQ(run.shown.front(), 20);
    CHECK_EQ(run.shown.back(), 299);
    CHECK(!clock.playing());  // the last frame stays up
    CHECK(clock.ended());
    CHECK_EQ(clock.shown(), 299);
    // play again from the start of the overlap
    clock.Play();
    CHECK_EQ(clock.position(), 20);
    CHECK(!clock.ended());
}

// A stop (a freeze mark) holds the clock at the end of one frame's interval, whatever the display's rate, and playing
// goes on from there when it is cleared. Without one nothing changes.
void stop_at_a_frame() {
    const Timeline t = sample_timeline();
    for (double hz : {120.0, 60.0, 30.0, 15.0}) {  // at 15 Hz the display skips three frames per tick
        Lockstep clock;
        clock.SetTimeline(t);
        clock.Seek(1000);
        SimDecoders sim(t, 500.0, 500.0, 1000);
        clock.Play();
        clock.SetStop(1100);
        double now = 0.0;
        Run run = play(clock, sim, hz, 4.0, 0.0, &now);
        CHECK_EQ(run.unready, 0);
        CHECK_EQ(run.shown.back(), 1100);       // it got there, and
        CHECK_EQ(clock.shown(), 1100);          // never went past it
        for (int64_t pair : run.shown)
            CHECK(pair <= 1100);
        CHECK(clock.playing());
        CHECK_EQ(clock.position(), 1100);       // the clock waits inside the frame's interval
        CHECK(clock.position_frames() >= 1101.0 - 1e-5 && clock.position_frames() < 1101.0);
        CHECK(!clock.buffering());
        // cleared: it moves on from the next frame, no jump back and no skip of the frame after the stop
        clock.SetStop(NO_PAIR);
        Run after = play(clock, sim, hz, 1.0, now);
        CHECK_EQ(after.unready, 0);
        CHECK(!after.shown.empty() && after.shown.front() >= 1101);
        if (hz >= 60.0)
            CHECK_EQ(after.shown.front(), 1101);
    }
    // a stop behind the clock does nothing
    {
        Lockstep clock;
        clock.SetTimeline(t);
        clock.Seek(2000);
        SimDecoders sim(t, 500.0, 500.0, 2000);
        clock.Play();
        clock.SetStop(1500);
        const Run run = play(clock, sim, 120.0, 1.0);
        CHECK(run.shown.back() > 2050);
    }
    // a stop on the last frame: the clock ends there as it always does
    {
        const Timeline short_timeline(300, 200, 20, t.fps);
        Lockstep clock;
        clock.SetTimeline(short_timeline);
        clock.SetStop(299);
        SimDecoders sim(short_timeline, 500.0, 500.0);
        clock.Play();
        const Run run = play(clock, sim, 120.0, 10.0);
        CHECK_EQ(run.shown.back(), 299);
        CHECK(!clock.playing() && clock.ended());
    }
}

void rate_controller() {
    RateController r;
    r.SetRequested(1.0);
    CHECK_NEAR(r.effective(), 1.0, 1e-12);
    // unknown capability: no limit
    r.Update(0.0, 60.0, 0.0, false);
    CHECK_NEAR(r.effective(), 1.0, 1e-12);
    // 45 pairs per second at 60 fps: 0.75 times the margin, rounded down to a step
    r.Update(1.0, 60.0, 45.0, false);
    CHECK_NEAR(r.effective(), 0.65, 1e-9);
    CHECK(r.limited());
    // a user speed below the limit is not limited
    r.SetRequested(0.5);
    CHECK_NEAR(r.effective(), 0.5, 1e-12);
    CHECK(!r.limited());
    // 2x asks for 120 pairs per second: the last estimate (0.65) holds it until decoding is measured again, then it climbs
    // a step each second to what 100 decode
    r.SetRequested(2.0);
    CHECK_NEAR(r.effective(), 0.65, 1e-9);
    for (double t = 2.0; t <= 20.0; t += 0.5)
        r.Update(t, 60.0, 100.0, false);
    CHECK_NEAR(r.effective(), 1.5, 1e-9);
    // a stall lowers it at once
    r.Update(21.0, 60.0, 100.0, true);
    CHECK_NEAR(r.effective(), 1.35, 1e-9);
    // decoders ahead of playback measure nothing (capability 0): the limit is tried upward a step per calm second
    RateController probe;
    probe.SetRequested(1.0);
    probe.Update(0.0, 60.0, 30.0, false);  // 30 pairs per second: 0.45
    CHECK_NEAR(probe.effective(), 0.45, 1e-9);
    for (double t = 1.0; t <= 30.0; t += 0.5)
        probe.Update(t, 60.0, 0.0, false);
    CHECK_NEAR(probe.effective(), 1.0, 1e-9);
    // a stall while probing holds it back at what it was
    probe.Update(31.0, 60.0, 0.0, true);
    CHECK(probe.effective() < 1.0);
    // never below the floor
    RateController low;
    low.SetRequested(1.0);
    low.Update(0.0, 60.0, 1.0, false);
    CHECK_NEAR(low.effective(), RateController::MIN_RATE, 1e-12);
}

}  // namespace


// --- Offset maps: K(n) ---------------------------------------------------------------------------------------------------

std::vector<OffsetPoint> points(std::initializer_list<OffsetPoint> list) {
    return std::vector<OffsetPoint>(list);
}

// K(n): linear between control points, rounded to the nearest (halves up), constant before the first and after the last; a
// jump is two points one frame apart.
void offset_map_values() {
    const OffsetMap constant(413);
    CHECK(constant.is_constant());
    CHECK_EQ(constant.at(-50), 413);
    CHECK_EQ(constant.at(9000), 413);
    CHECK_EQ(constant.first_k(), 413);
    CHECK_EQ(constant.min_k(), 413);
    const OffsetMap m(points({{100, 10.0}, {200, 12.0}, {500, 12.0}, {501, 20.0}}));
    CHECK(!m.is_constant());
    CHECK_EQ(m.at(0), 10);  // constant before the first point
    CHECK_EQ(m.at(100), 10);
    CHECK_EQ(m.at(124), 10);  // 10.48
    CHECK_EQ(m.at(125), 11);  // 10.5 rounds up
    CHECK_EQ(m.at(150), 11);
    CHECK_EQ(m.at(200), 12);
    CHECK_EQ(m.at(499), 12);
    CHECK_EQ(m.at(500), 12);
    CHECK_EQ(m.at(501), 20);  // the jump takes one frame
    CHECK_EQ(m.at(9999), 20);  // constant after the last
    CHECK_EQ(m.first_k(), 10);
    CHECK_EQ(m.min_k(), 10);
    CHECK_EQ(m.max_k(), 20);
    CHECK_EQ((int)m.jumps().size(), 1);
    CHECK_EQ(m.jumps()[0], 501);
    CHECK_NEAR(m.slope_at(150), 0.02, 1e-12);
    CHECK_NEAR(m.slope_at(300), 0.0, 1e-12);
    CHECK_NEAR(m.slope_at(500), 0.0, 1e-12);  // a jump has no slope
    const OffsetMap negative(points({{0, -3.5}, {10, -3.5}}));
    CHECK_EQ(negative.at(5), -3);  // halves up for negative K too
    const OffsetMap unsorted(points({{200, 12.0}, {100, 10.0}}));
    CHECK_EQ(unsorted.points().front().a, 100);
    CHECK_EQ(unsorted.first_k(), 10);
    CHECK_EQ(m.b_frame(150), 150 - 11);
}

// The anchors: a nudge moves the nearest anchor within a second or makes one at the playhead; Add replaces one at its frame.
void offset_map_edits() {
    OffsetMap m(400);
    m.Nudge(1000, +2, 60);  // no anchors: the first one, and K is constant at its value
    CHECK_EQ((int)m.points().size(), 1);
    CHECK_EQ(m.at(0), 402);
    CHECK_EQ(m.at(5000), 402);
    m.Nudge(1030, +1, 60);  // within a second: that anchor moves
    CHECK_EQ((int)m.points().size(), 1);
    CHECK_EQ(m.at(1000), 403);
    m.Nudge(3000, -1, 60);  // far from it: a new anchor with K(3000) - 1
    CHECK_EQ((int)m.points().size(), 2);
    CHECK_EQ(m.at(3000), 402);
    CHECK_EQ(m.at(2000), 403);  // 402.5
    m.Add(3000, 410.0);  // at an anchor's frame: it changes
    CHECK_EQ((int)m.points().size(), 2);
    CHECK_EQ(m.at(3000), 410);
    CHECK_EQ(m.Nearest(2900), 1);
    CHECK(m.RemoveNearest(2900));
    CHECK_EQ((int)m.points().size(), 1);
    CHECK_EQ(m.at(3000), 403);
    m.Clear();
    CHECK(m.is_constant());
    CHECK(!OffsetMap(5).RemoveNearest(0));
    // swapped: A frame n matched B frame m = n - K(n), so the new map has {m, -K}; twice gives the points back
    const OffsetMap line(points({{0, 400.0}, {1000, 410.0}}));
    const OffsetMap swapped = line.Swapped();
    CHECK_EQ((int)swapped.points().size(), 2);
    CHECK_EQ(swapped.points()[0].a, -400);
    CHECK_NEAR(swapped.points()[0].k, -400.0, 1e-12);
    CHECK_EQ(swapped.points()[1].a, 590);
    CHECK_NEAR(swapped.points()[1].k, -410.0, 1e-12);
    for (int64_t n = 0; n <= 1000; n += 7)
        CHECK(std::llabs(swapped.at(n - line.at(n)) + line.at(n)) <= 1);  // the same moments, up to the rounding
    CHECK(swapped.Swapped() == line);
    CHECK(OffsetMap(413).Swapped() == OffsetMap(-413));
}

// The timeline over a map (a slope of one frame in 250 and a pause at 1500 after which B is 20 frames further on): the
// extent of both recordings and of the overlap against a scan of every frame.
OffsetMap drift_map() {
    return OffsetMap(points({{0, 400.0}, {1500, 406.0}, {1501, 386.0}, {2900, 391.6}}));
}

void drifting_timeline() {
    const Timeline t(3000, 2700, drift_map(), 60.0);
    int64_t both_first = INT64_MAX, both_last = INT64_MIN, b_first = INT64_MAX, b_last = INT64_MIN;
    for (int64_t n = -1000; n < 5000; n++) {
        if (t.has_a(n) && t.has_b(n)) {
            both_first = std::min(both_first, n);
            both_last = std::max(both_last, n);
        }
        if (t.b_frame(n) >= 0)
            b_first = std::min(b_first, n);
        if (t.b_frame(n) <= t.frames_b - 1)
            b_last = std::max(b_last, n);
    }
    CHECK_EQ(t.overlap_first(), both_first);
    CHECK_EQ(t.overlap_end(), both_last + 1);
    CHECK_EQ(t.first(), std::min<int64_t>(0, b_first));
    CHECK_EQ(t.end(), std::max<int64_t>(t.frames_a, b_last + 1));
    CHECK(t.has_overlap());
    CHECK_EQ(t.offset(1000), 404);
    CHECK_EQ(t.offset(1500), 406);
    CHECK_EQ(t.offset(1501), 386);
    // the B frames the pairs of [n0, n1] need: around a small jump both sides; the long ones are cut (K falls by 20 here)
    const Timeline::FrameRange small = t.b_range(1400, 1450, 1600, 120);
    CHECK(small.first <= t.b_frame(1400) && small.last >= t.b_frame(1600) && small.last >= t.b_frame(1501));
    const Timeline::FrameRange cut = t.b_range(1400, 1450, 1600, 10);  // a jump of 20 is more than 10: not decoded through
    CHECK(cut.last >= t.b_frame(1500) && cut.last < t.b_frame(1501));
    const Timeline::FrameRange cut_behind = t.b_range(1400, 1550, 1600, 10);  // the playhead after it: the part before is dropped
    CHECK(cut_behind.first >= t.b_frame(1501) - 1 && cut_behind.last >= t.b_frame(1600));
    CHECK_EQ(cut_behind.head, t.b_frame(1550));
    // at the last frame before the cut jump the window is the one beyond it, with the playhead at its far side (nothing of the
    // near side is needed any more; the clock waits for the first pair after the jump): B seeks
    const Timeline::FrameRange lead = t.b_range(1400, 1500, 1540, 10);
    CHECK(lead.first >= t.b_frame(1501) - 1 && lead.last >= t.b_frame(1540));
    CHECK_EQ(lead.head, t.b_frame(1501));
    const Timeline::FrameRange early = t.b_range(1400, 1498, 1525, 10);  // two frames before it: the near side still
    CHECK_EQ(early.head, t.b_frame(1498));
    CHECK(early.last < t.b_frame(1501));
}

// The pairing over a map: B repeats a frame where K rises by 1, skips one where it falls by 1, and is 21 frames further on
// after the jump; every pair has B frame n - K(n), the decoders keep up with B's moving position, and the edit list (the
// export's source of pairs, with a freeze that repeats its pair) pairs the same frames as the playback.
void drifting_pairs() {
    const Timeline t(3000, 2700, drift_map(), 60.0);
    Lockstep clock;
    clock.SetTimeline(t);
    CHECK_EQ(clock.position(), t.overlap_first());
    const int64_t start = 1000;
    clock.Seek(start);
    SimDecoders sim(t, 240.0, 240.0, start);
    clock.Play();
    std::map<int64_t, int64_t> b_of;  // pair -> B frame, as played
    int unready = 0, repeats = 0, skips = 0, jumps = 0, mismatched = 0;
    int64_t previous_pair = NO_PAIR, previous_b = -1;
    double now = 0.0;
    for (int i = 0; i < 120 * 12; i++, now += 1.0 / 120.0) {
        sim.Step(now, 1.0 / 120.0, clock.position());
        const Lockstep::Decision d = clock.Tick(now, sim, sim.Capability());
        if (!d.changed)
            continue;
        mismatched += !d.has_a || !d.has_b || d.b_frame != t.b_frame(d.pair) || d.b_frame != d.pair - t.offset(d.pair);
        unready += !sim.Ready(0, d.pair) || !sim.Ready(1, d.b_frame);
        b_of[d.pair] = d.b_frame;
        if (previous_pair != NO_PAIR && d.pair == previous_pair + 1) {
            const int64_t step = d.b_frame - previous_b;
            repeats += step == 0;
            skips += step == 2;
            jumps += step > 2 || step < 0;
        }
        previous_pair = d.pair;
        previous_b = d.b_frame;
    }
    CHECK(previous_pair > 1700);  // played through the pause at 1500
    CHECK_EQ(mismatched, 0);
    CHECK_EQ(unready, 0);
    // K rises 0.004 per frame (a repeat at 1125 and at 1375), then the jump takes B 21 ahead (one step of 22), and a repeat
    // at 1626
    CHECK_EQ(jumps, 1);
    CHECK_EQ(repeats, 3);
    CHECK_EQ(skips, 0);
    CHECK_EQ(b_of[1500], 1500 - 406);
    CHECK_EQ(b_of[1501], 1501 - 386);
    // the export's pairs: the edit list's pair for each output frame, B from the same K(n)
    std::vector<Area> areas;
    Area freeze;
    freeze.id = 1;
    freeze.kind = AreaKind::FREEZE;
    freeze.t0 = freeze.t1 = 1500.0 / 60.0;
    freeze.rect[2] = freeze.rect[3] = 0.25;
    areas.push_back(freeze);
    EditOptions options;
    options.fps = 60.0;
    options.first = 1400;
    options.end = 1700;
    const EditList edit(areas, options);
    CHECK(edit.length() > 300);  // the freeze adds its frames
    int64_t frozen_b = -1;
    for (int64_t i = 0; i < edit.length(); i++) {
        const EditEntry e = edit.at(i);
        CHECK(b_of.count(e.pair) > 0);
        CHECK_EQ(t.b_frame(e.pair), b_of[e.pair]);
        if (e.pair == 1500) {  // the frozen pair is one pair of both recordings, however long it stays
            frozen_b = frozen_b < 0 ? t.b_frame(e.pair) : frozen_b;
            CHECK_EQ(t.b_frame(e.pair), frozen_b);
        }
    }
    CHECK_EQ(frozen_b, 1500 - 406);
}

// A nudge or a map change keeps the position and shows the pair of the new K once its frames are decoded.
void map_change() {
    const Timeline t(3000, 2700, 400, 60.0);
    Lockstep clock;
    clock.SetTimeline(t);
    SimDecoders sim(t, 240.0, 240.0);
    clock.Seek(1000);
    sim.Restart(0, 1000 - 60, 0.0, 0.0);
    sim.Restart(1, t.b_frame(1000) - 60, 0.0, 0.0);
    sim.Step(0.0, 1.0, 1000);
    Lockstep::Decision d = clock.Tick(0.0, sim);
    CHECK_EQ(d.b_frame, 600);
    // anchors: K is 400 until 1000 and 402 from there
    OffsetMap map(points({{1000, 400.0}, {1001, 402.0}}));
    clock.SetOffsets(map);
    CHECK(clock.timeline().offsets() == map);
    d = clock.Tick(0.1, sim);
    CHECK_EQ(d.pair, 1000);
    CHECK_EQ(d.b_frame, 600);
    clock.Seek(1001);
    d = clock.Tick(0.2, sim);
    CHECK_EQ(d.pair, 1001);
    CHECK_EQ(d.b_frame, 1001 - 402);
}

int main() {
    timeline_geometry();
    healthy_decoders();
    decode_limited();
    stalling_decoder();
    pairs_never_mismatch_across_long_play();
    seeks_and_steps();
    one_recording_only();
    offset_change();
    offset_map_values();
    offset_map_edits();
    drifting_timeline();
    drifting_pairs();
    map_change();
    end_of_timeline();
    stop_at_a_frame();
    rate_controller();
    return check_result("lockstep");
}
