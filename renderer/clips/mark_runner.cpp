#include "renderer/clips/mark_runner.h"

#include <algorithm>
#include <cmath>

namespace renderer::clips {
namespace {

// The next mark to run: the first at or after the position, past those that already ran at this frame.
const FreezeSequence* NextMark(const EditList& marks, int64_t position, int64_t done_frame, int done_count) {
    const std::vector<FreezeSequence>& f = marks.freezes();
    size_t i = 0;
    while (i < f.size() && f[i].frame < position)
        i++;
    for (int k = 0; k < done_count && i < f.size() && f[i].frame == done_frame && done_frame == position; k++)
        i++;
    return i < f.size() ? &f[i] : nullptr;
}

bool Has(const EditList& marks, int area_id) {
    for (const FreezeSequence& f : marks.freezes())
        if (f.area_id == area_id)
            return true;
    return false;
}

}  // namespace

void MarkRunner::BeforeTick(Lockstep& clock, const EditList& marks) {
    if (running_ || pending_)
        return;  // the stop that holds the clock stays
    if (clock.position() != done_frame_) {
        done_frame_ = NO_PAIR;
        done_count_ = 0;
    }
    if (!enabled_ || !clock.playing()) {
        clock.SetStop(NO_PAIR);
        return;
    }
    const FreezeSequence* next = NextMark(marks, clock.position(), done_frame_, done_count_);
    clock.SetStop(next ? next->frame : NO_PAIR);
}

void MarkRunner::StartPreview(const FreezeSequence& sequence, Lockstep& clock) {
    if (sequence.length() <= 0)
        return;
    clock.Pause();
    clock.SetStop(NO_PAIR);
    clock.Seek(sequence.frame);
    sequence_ = sequence;
    pending_ = true;
    running_ = false;
    preview_ = true;
}

void MarkRunner::Abort(Lockstep& clock) {
    (void)clock;
    running_ = false;
    pending_ = false;
}

MarkRunner::State MarkRunner::AfterTick(double now, Lockstep& clock, const EditList& marks, const Shot& live) {
    const double dt = last_now_ < 0.0 ? 0.0 : std::clamp(now - last_now_, 0.0, 0.1);
    last_now_ = now;
    const int64_t position = clock.position();
    if (position != done_frame_) {
        done_frame_ = NO_PAIR;
        done_count_ = 0;
    }
    bool fresh = false;  // started on this call: no time has run yet
    if (pending_) {
        if (position != sequence_.frame) {
            pending_ = false;  // the viewer moved away before the frame came up
        } else if (clock.shown() == sequence_.frame) {
            pending_ = false;
            running_ = true;
            preview_ = true;
            base_ = live;
            progress_ = 0.0;
            fresh = true;
        }
    }
    if (!running_ && !pending_ && enabled_ && (clock.playing() || clock.ended())) {
        const FreezeSequence* next = NextMark(marks, position, done_frame_, done_count_);
        if (next && position == next->frame && clock.shown() == next->frame) {
            sequence_ = *next;
            running_ = true;
            preview_ = false;
            base_ = live;
            // A second mark at the frame goes on from the first one's last step (the base camera): its step 0 is that one.
            progress_ = done_frame_ == next->frame && done_count_ > 0 ? 1.0 : 0.0;
            fresh = true;
        }
    }
    State state;
    if (!running_)
        return state;
    // The sequence goes on while its frame is the one on screen; a seek or a deleted mark ends it.
    const bool alive = position == sequence_.frame && clock.shown() == sequence_.frame && (preview_ || Has(marks, sequence_.area_id));
    if (!alive || (!preview_ && !enabled_)) {
        running_ = false;
        return state;
    }
    if (!fresh && (preview_ || clock.playing() || clock.ended()))
        progress_ += dt * marks.options().fps * clock.speed();
    const int64_t step = (int64_t)std::floor(progress_);
    if (step > sequence_.length()) {
        running_ = false;
        if (!preview_) {  // the next mark at this frame, if any, starts on the next call
            if (done_frame_ != sequence_.frame) {
                done_frame_ = sequence_.frame;
                done_count_ = 0;
            }
            done_count_++;
        }
        return state;
    }
    state.active = true;
    state.shot = freeze_shot(sequence_, base_, step);
    state.area_id = sequence_.area_id;
    state.phase = freeze_phase(sequence_, step);
    state.step = step;
    state.length = sequence_.length();
    state.preview = preview_;
    return state;
}

}  // namespace renderer::clips
