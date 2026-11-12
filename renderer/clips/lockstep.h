// The clock and the pairing of two recordings: pure logic (no GPU, no OS), tested by lockstep_test.cpp with a simulated
// slow and stalling decoder.
//
// Frame n of A shows the same moment as frame n - K(n) of B (K = the offset in frames, a whole number at each frame: one
// constant, or the project's offset map when the recordings drift, offset_map.h). The timeline is in A's numbering: it
// runs from min(0, where B's first frame is) to max(frames_a, where B ends), so the frames where only one recording
// exists are part of it. One master clock moves a position (in frames) at the play rate times the frame rate. The pair on
// screen is the position's frame n with n - K(n) of B, shown only when both frames are decoded and uploaded:
//
//  - If either is late, the pair on screen is held, the status is "buffering", and the clock waits at the end of the
//    shown frame's interval: playback resumes from there without catching up, and a pair is never mismatched.
//  - If the display is late instead (a hitch, or a play rate above the display's), the newest ready pair at or before
//    the clock's frame is shown, never a later one.
//  - The play rate is lowered automatically (RateController) when the decoders cannot sustain it, and raised again
//    when they can.
//  - Where K changes by 1, B shows a frame twice (K up) or skips one (K down); at a jump B is somewhere else, and the pair
//    waits for that frame like for any other (the player keeps each decoder's window on B's moving position).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "renderer/clips/offset_map.h"

namespace renderer::clips {

inline constexpr int64_t NO_PAIR = INT64_MIN;  // no pair on screen yet (a negative frame number is real when K < 0)

// The frames of both recordings and K(n). Built whole: first, end and the overlap are computed once, here.
class Timeline {
public:
    Timeline() = default;
    Timeline(int64_t frames_a_, int64_t frames_b_, const OffsetMap& offsets, double fps_ = 60.0)
        : frames_a(frames_a_), frames_b(frames_b_), fps(fps_), offsets_(offsets) {
        Compute();
    }

    int64_t frames_a = 0, frames_b = 0;
    double fps = 60.0;

    const OffsetMap& offsets() const { return offsets_; }
    // K at A frame n, and B's frame there.
    int64_t offset(int64_t n) const { return offsets_.at(n); }
    int64_t b_frame(int64_t n) const { return n - offsets_.at(n); }
    // The same recordings with another offset map.
    Timeline WithOffsets(const OffsetMap& offsets) const { return Timeline(frames_a, frames_b, offsets, fps); }

    int64_t first() const { return first_; }
    int64_t end() const { return end_; }  // one past the last
    bool has_a(int64_t n) const { return n >= 0 && n < frames_a; }
    bool has_b(int64_t n) const {
        const int64_t m = b_frame(n);
        return m >= 0 && m < frames_b;
    }
    int64_t overlap_first() const { return overlap_first_; }
    int64_t overlap_end() const { return overlap_end_; }  // may be before overlap_first
    bool has_overlap() const { return overlap_end_ > overlap_first_; }

    // The frames of B that the pairs of A frames [n0, n1] need, around the playhead n (n0 <= n <= n1): the lowest and
    // highest B frame, clamped to B, and the frame the decoder should treat as the playhead. A jump of more than max_gap
    // frames in K ends the range on that side, so a decoder is not asked to decode through a long pause or hitch. At the
    // last frame before such a jump (`lead` frames before it; negative: never) the range is the one beyond it, with the
    // playhead at the jump's far side: B seeks there (the caller asks for it when nothing of the near side is needed any
    // more, the clock waiting for the first pair after the jump). last < first: B has no frame there.
    struct FrameRange {
        int64_t first = 0, last = -1, head = 0;
    };
    FrameRange b_range(int64_t n0, int64_t n, int64_t n1, int64_t max_gap, int64_t lead = 1) const;

private:
    void Compute();

    OffsetMap offsets_;
    int64_t first_ = 0, end_ = 0, overlap_first_ = 0, overlap_end_ = 0;
};

// Which frames are decoded, uploaded and ready to show.
class Availability {
public:
    virtual ~Availability() = default;
    virtual bool Ready(int video, int64_t frame) const = 0;  // video 0 = A, 1 = B; the video's own frame number
};

// The play rate: the one the user asked for, lowered to what the decoders sustain. Decoder throughput is the pairs
// per second both can decode (0: unknown). Lowering is immediate; raising takes calm, so the rate does not oscillate.
class RateController {
public:
    static constexpr double MIN_RATE = 0.1;
    static constexpr double MARGIN = 0.92;       // the rate may use this fraction of the decode throughput
    static constexpr double RAISE_AFTER = 1.0;   // seconds without a stall or a lowering before a step up
    static constexpr double STEP = 0.05;

    void SetRequested(double rate) {
        requested_ = std::clamp(rate, MIN_RATE, 8.0);
        effective_ = std::min(requested_, limit_);
    }
    double requested() const { return requested_; }
    double effective() const { return effective_; }
    bool limited() const { return effective_ < requested_ - 0.005; }

    // Every tick that plays. capability: decoded pairs per second while the decoders are the bottleneck (0: not
    // measurable now, because they are ahead: the last estimate holds and is tried upward, a step per calm period); fps:
    // the frame rate; stalled: a new stall began on this tick.
    void Update(double now, double fps, double capability, bool stalled) {
        if (capability > 0.0 && fps > 0.0) {
            limit_ = std::max(std::floor(capability / fps * MARGIN / STEP) * STEP, MIN_RATE);
        } else if (limit_ < requested_ && now - probed_ >= RAISE_AFTER && now - changed_ >= RAISE_AFTER) {
            limit_ = std::min(requested_, limit_ + STEP);  // the decoders keep up: try a little more
            probed_ = now;
        }
        const double want = std::min(requested_, limit_);
        if (stalled) {
            effective_ = std::max(MIN_RATE, std::min(effective_, want) * 0.9);
            limit_ = std::min(limit_, std::max(effective_, MIN_RATE));
            changed_ = now;
        } else if (want < effective_ - 0.02) {
            effective_ = want;
            changed_ = now;
        } else if (want > effective_ + 1e-6 && now - changed_ >= RAISE_AFTER) {
            effective_ = std::min(want, effective_ + STEP);
            changed_ = now;
        }
    }

private:
    double requested_ = 1.0, effective_ = 1.0, limit_ = 1e9, changed_ = 0.0, probed_ = 0.0;
};

struct LockstepStats {
    uint64_t pairs_shown = 0;  // new pairs put on screen
    uint64_t pairs_held = 0;   // ticks on which a new pair was due and the shown one was held
    uint64_t stalls = 0;       // playing, a new pair due and not ready: episodes that came after the first pair
    double rate_min = 0.0;     // the lowest effective rate while playing (0: never played)
};

class Lockstep {
public:
    struct Decision {
        int64_t pair = NO_PAIR;  // the A frame number of the pair to show; NO_PAIR: nothing yet
        bool changed = false;  // a new pair: the caller shows it (acquires both frames)
        bool buffering = false;
        bool has_a = false, has_b = false;
        int64_t b_frame = -1;  // pair - K(pair) when B exists
    };

    void SetTimeline(const Timeline& t);
    const Timeline& timeline() const { return timeline_; }
    // Another offset (a nudge or a detection), a constant or a map: the position stays, the pair shown follows it.
    void SetOffsets(const OffsetMap& offsets);
    void SetOffset(int64_t k) { SetOffsets(OffsetMap(k)); }

    // The caller could not take the pair Tick() reported (a frame went missing): show it again when it is ready.
    void Invalidate() { shown_valid_ = false; }

    void Play();
    void Pause() { playing_ = false; }
    bool playing() const { return playing_; }
    void Seek(int64_t frame);    // the position; the shown pair stays until the new one is ready
    void Step(int64_t delta) {   // pause and move
        Pause();
        Seek(position() + delta);
    }
    // A stop (a freeze mark): while playing, the clock does not go past the end of this frame's interval, so the pair
    // stays on screen with the clock waiting at its end (position at most frame + 1 - epsilon). The clock is still
    // playing: it moves on when the stop is cleared (NO_PAIR). A clock already past the frame is not moved back.
    void SetStop(int64_t frame) { stop_ = frame; }
    int64_t stop() const { return stop_; }
    // The clock stopped itself at the end of the timeline (the last frame stays up); cleared by Play and Seek.
    bool ended() const { return ended_; }
    void SetSpeed(double speed) { rate_.SetRequested(speed); }
    double speed() const { return rate_.requested(); }
    double rate() const { return rate_.effective(); }
    bool decode_limited() const { return rate_.limited(); }

    int64_t position() const { return (int64_t)std::floor(position_); }
    double position_frames() const { return position_; }
    int64_t shown() const { return shown_; }  // NO_PAIR: none
    bool buffering() const { return buffering_; }
    const LockstepStats& stats() const { return stats_; }

    // One tick of the display loop. now: seconds on a monotonic clock. capability: decoded pairs per second (0:
    // unknown).
    Decision Tick(double now, const Availability& availability, double capability = 0.0);

private:
    bool PairReady(const Availability& availability, int64_t n) const;
    Decision Show(int64_t n, bool changed) const;

    Timeline timeline_;
    RateController rate_;
    LockstepStats stats_;
    double position_ = 0.0;
    double last_now_ = -1.0;
    int64_t shown_ = NO_PAIR;
    bool shown_valid_ = false;  // shown_ is on screen and follows the current offset
    bool playing_ = false;
    bool buffering_ = false;
    bool stalled_ = false;      // in a stall episode (a new pair due, none ready)
    bool played_since_seek_ = false;  // a pair was shown since the last play or seek, so a wait is a stall
    int64_t stop_ = NO_PAIR;
    bool ended_ = false;
    int64_t anchor_ = 0;        // the first frame a pair may show since the last play, seek or offset change
};

}  // namespace renderer::clips
