#include "renderer/clips/lockstep.h"

#include <cstdlib>
#include <vector>

namespace renderer::clips {

void Lockstep::SetTimeline(const Timeline& t) {
    timeline_ = t;
    position_ = t.has_overlap() ? (double)t.overlap_first() : (double)t.first();
    anchor_ = position();
    shown_ = NO_PAIR;
    shown_valid_ = false;
    played_since_seek_ = false;
    stalled_ = false;
    buffering_ = false;
    ended_ = false;
    last_now_ = -1.0;
}

void Timeline::Compute() {
    // B's first frame and one past its last, in A's numbering: the n where n - K(n) first reaches 0, and the last n whose
    // B frame is B's last. K stays within [min, max], so both lie in a short range, found by a scan.
    int64_t b_first = offsets_.first_k(), b_end = frames_b + offsets_.first_k();
    if (!offsets_.is_constant()) {
        const int64_t lo = offsets_.min_k(), hi = offsets_.max_k();
        b_first = hi;
        for (int64_t n = lo; n <= hi; n++)
            if (n - offsets_.at(n) >= 0) {
                b_first = n;
                break;
            }
        b_end = frames_b + lo;
        for (int64_t n = frames_b - 1 + hi; n >= frames_b - 1 + lo; n--)
            if (n - offsets_.at(n) <= frames_b - 1) {
                b_end = n + 1;
                break;
            }
    }
    first_ = std::min<int64_t>(0, b_first);
    end_ = std::max(frames_a, b_end);
    overlap_first_ = std::max<int64_t>(0, b_first);
    overlap_end_ = std::min(frames_a, b_end);
}

Timeline::FrameRange Timeline::b_range(int64_t n0, int64_t n, int64_t n1, int64_t max_gap, int64_t lead) const {
    // K is piecewise linear, so n - K(n) has its extremes at the ends and at the control points in between. Walk these
    // from the playhead out to each side and stop at a jump.
    std::vector<int64_t> at = {n0, n, n1};
    for (const OffsetPoint& p : offsets_.points())
        if (p.a > n0 && p.a < n1)
            at.push_back(p.a);
    std::sort(at.begin(), at.end());
    at.erase(std::unique(at.begin(), at.end()), at.end());
    size_t lo = (size_t)(std::find(at.begin(), at.end(), n) - at.begin());
    for (size_t i = lo; i + 1 < at.size(); i++)  // the first long jump ahead: within `lead` frames, the playhead is past it
        if (std::llabs(offset(at[i + 1]) - offset(at[i])) > max_gap) {
            if (at[i + 1] - n <= lead)
                lo = i + 1;
            break;
        }
    size_t hi = lo;
    const int64_t head_frame = b_frame(at[lo]);
    while (hi + 1 < at.size() && std::llabs(offset(at[hi + 1]) - offset(at[hi])) <= max_gap)
        hi++;
    while (lo > 0 && std::llabs(offset(at[lo - 1]) - offset(at[lo])) <= max_gap)
        lo--;
    int64_t first = INT64_MAX, last = INT64_MIN;
    for (size_t i = lo; i <= hi; i++) {
        first = std::min(first, b_frame(at[i]));
        last = std::max(last, b_frame(at[i]));
    }
    // the rounding of K inside a segment can reach a frame beyond its ends
    FrameRange range;
    range.first = std::max<int64_t>(first - 1, 0);
    range.last = std::min<int64_t>(last + 1, frames_b - 1);
    range.head = std::clamp<int64_t>(head_frame, 0, std::max<int64_t>(frames_b - 1, 0));
    return range;
}

void Lockstep::SetOffsets(const OffsetMap& offsets) {
    if (offsets == timeline_.offsets())
        return;
    timeline_ = timeline_.WithOffsets(offsets);
    position_ = std::clamp(position_, (double)timeline_.first(), (double)timeline_.end() - 1e-6);
    // The pair on screen was made with the old offset and stays up until the new one is ready.
    shown_valid_ = false;
    anchor_ = position();
    played_since_seek_ = false;
    stalled_ = false;
}

void Lockstep::Play() {
    if (playing_)
        return;
    if (position() >= timeline_.end() - 1)  // at the end: from the start of the overlap
        Seek(timeline_.has_overlap() ? timeline_.overlap_first() : timeline_.first());
    playing_ = true;
    ended_ = false;
    anchor_ = position();
    played_since_seek_ = false;
    stalled_ = false;
    last_now_ = -1.0;
}

void Lockstep::Seek(int64_t frame) {
    frame = std::clamp<int64_t>(frame, timeline_.first(), std::max(timeline_.end() - 1, timeline_.first()));
    position_ = (double)frame;
    anchor_ = frame;
    ended_ = false;
    played_since_seek_ = false;
    stalled_ = false;
    if (shown_ != frame)
        shown_valid_ = false;
}

bool Lockstep::PairReady(const Availability& availability, int64_t n) const {
    return (!timeline_.has_a(n) || availability.Ready(0, n)) &&
           (!timeline_.has_b(n) || availability.Ready(1, timeline_.b_frame(n)));
}

Lockstep::Decision Lockstep::Show(int64_t n, bool changed) const {
    Decision d;
    d.pair = n;
    d.changed = changed;
    d.buffering = buffering_;
    if (n != NO_PAIR) {
        d.has_a = timeline_.has_a(n);
        d.has_b = timeline_.has_b(n);
        d.b_frame = d.has_b ? timeline_.b_frame(n) : -1;
    }
    return d;
}

Lockstep::Decision Lockstep::Tick(double now, const Availability& availability, double capability) {
    const double dt = last_now_ < 0.0 ? 0.0 : std::clamp(now - last_now_, 0.0, 0.1);
    last_now_ = now;
    buffering_ = false;
    if (timeline_.end() <= timeline_.first())
        return Decision();
    const int64_t first = timeline_.first(), end = timeline_.end();

    if (!playing_) {
        const int64_t want = position();
        if (shown_valid_ && shown_ == want)
            return Show(shown_, false);
        if (PairReady(availability, want)) {
            shown_ = want;
            shown_valid_ = true;
            stats_.pairs_shown++;
            return Show(shown_, true);
        }
        buffering_ = true;
        stats_.pairs_held++;
        return Show(shown_, false);
    }

    // Playing. The clock advances while a pair of this run is on screen; before the first one it waits.
    const bool running = shown_valid_ && shown_ >= anchor_;
    double next = position_ + (running ? rate_.effective() * timeline_.fps * dt : 0.0);
    if (stop_ != NO_PAIR && position_ <= (double)(stop_ + 1) - 1e-6)  // a clock already past the stop is left alone
        next = std::min(next, (double)(stop_ + 1) - 1e-6);
    bool ended = false;
    if (next >= (double)end - 1e-6) {
        next = (double)end - 1e-6;
        ended = true;
    }
    next = std::max(next, (double)first);
    const int64_t due = (int64_t)std::floor(next);
    bool stall_began = false;
    Decision decision;
    if (shown_valid_ && due == shown_) {
        position_ = next;
        decision = Show(shown_, false);
    } else {
        // The newest ready pair in (the pair on screen, due], none before the anchor.
        const int64_t lowest = running ? shown_ + 1 : anchor_;
        int64_t found = NO_PAIR;
        for (int64_t c = due; c >= lowest && c >= first; c--)
            if (PairReady(availability, c)) {
                found = c;
                break;
            }
        if (found != NO_PAIR) {
            shown_ = found;
            shown_valid_ = true;
            position_ = next;
            stalled_ = false;
            played_since_seek_ = true;
            stats_.pairs_shown++;
            decision = Show(found, true);
        } else {
            // Late: hold the pair on screen and wait where the clock is (at the end of the shown frame's interval).
            buffering_ = true;
            stats_.pairs_held++;
            if (!stalled_ && played_since_seek_) {
                stats_.stalls++;
                stall_began = true;
            }
            stalled_ = true;
            if (running)
                position_ = std::min(next, (double)(shown_ + 1) - 1e-6);
            decision = Show(shown_, false);
        }
    }
    rate_.Update(now, timeline_.fps, capability, stall_began);
    stats_.rate_min = stats_.rate_min == 0.0 ? rate_.effective() : std::min(stats_.rate_min, rate_.effective());
    if (ended && shown_ == end - 1 && shown_valid_) {
        playing_ = false;  // the last frame stays up
        ended_ = true;
        position_ = (double)(end - 1);
    }
    return decision;
}

}  // namespace renderer::clips
