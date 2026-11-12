// Freeze marks in the player: while playing, the clock stops at a freeze mark's frame, the sequence of the edit list
// (edit_list.h: zoom in, hold, zoom out, over the pair that stays on screen) runs in real time, and playing goes on by
// itself. Pure logic over the Lockstep clock (no GPU, no OS), tested by mark_runner_test.cpp against the edit list.
//
// The clock is held with Lockstep::SetStop, not paused: it keeps its pair on screen, keeps playing (so the decoders keep
// working ahead of the playhead during the sequence and playing resumes without a stall) and waits at the end of the
// frame's interval. The pairing, the buffering and the rate logic are the clock's own.
//
// Each UI frame the viewer calls BeforeTick (sets the stop before the clock moves), Player::Update, then AfterTick
// (starts, advances and ends sequences, and returns the shot to show while one runs). Rules:
//  - Marks run only during playing, with the option on. Scrubbing, stepping and paused viewing do not run them, and a
//    seek during a sequence ends it.
//  - A sequence starts when its frame comes up on screen, so the frame is on screen for 1 + length() frame times in all,
//    as in the export. Playing from a marked frame (Space on it, a seek and play) runs the mark of that frame.
//  - Pausing during a sequence pauses it; it goes on with playing. Marks at one frame run one after the other.
//  - A mark's own preview (StartPreview) pauses at its frame and runs the sequence once, whatever the option says; the
//    clock stays paused after it.
//  - Time runs at the clock's speed (0.5x makes a sequence twice as long).

#pragma once

#include <cstdint>

#include "renderer/clips/edit_list.h"
#include "renderer/clips/lockstep.h"

namespace renderer::clips {

class MarkRunner {
public:
    struct State {
        bool active = false;        // a sequence is on screen: its shot replaces the viewer's own
        Shot shot;
        int area_id = 0;
        FreezePhase phase = FreezePhase::ZOOMING_IN;
        int64_t step = 0, length = 0;
        bool preview = false;
    };

    void SetEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    // Before the clock's tick: the stop at the next mark ahead (or none while paused or off).
    void BeforeTick(Lockstep& clock, const EditList& marks);
    // After it. now: seconds on a monotonic clock. live: the shot the viewer shows without a sequence (its camera,
    // divider and caption now), the base the sequence zooms from and back to.
    State AfterTick(double now, Lockstep& clock, const EditList& marks, const Shot& live);

    // Watch one mark's sequence from its start: pauses, goes to its frame and runs it once when the frame is up.
    void StartPreview(const FreezeSequence& sequence, Lockstep& clock);
    // Ends a sequence in progress (the shot goes back to the viewer's own).
    void Abort(Lockstep& clock);

    bool running() const { return running_ || pending_; }

private:
    bool enabled_ = true;
    bool running_ = false;
    bool pending_ = false;           // a preview waits for its frame
    bool preview_ = false;
    FreezeSequence sequence_;
    Shot base_;
    double progress_ = 0.0;          // frames of the sequence run so far (step = floor)
    double last_now_ = -1.0;
    int64_t done_frame_ = NO_PAIR;   // the frame whose marks have run since the clock came to it
    int done_count_ = 0;
};

}  // namespace renderer::clips
