#include "renderer/clips/player.h"

#include <algorithm>
#include <cmath>

namespace renderer::clips {

Player::Player(VideoSource* a, VideoSource* b, const OffsetMap& offsets, PlayerConfig config)
    : source_{a, b}, config_(config) {
    clock_.SetTimeline(Timeline(a->Info().frames, b->Info().frames, offsets, a->Info().fps > 0.0 ? a->Info().fps : 60.0));
}

void Player::SetOffsets(const OffsetMap& offsets) {
    clock_.SetOffsets(offsets);
}

double Player::capability() const {
    // A source that has the next frames ready is faster than playback: its decode rate then says what playback asked
    // for, not what it can do. One that is behind is working flat out.
    double rate = 0.0;
    const Timeline& t = clock_.timeline();
    for (int v = 0; v < 2; v++) {
        const VideoSource* s = source_[v];
        const int64_t frames = s->Info().frames;
        bool behind = false;
        for (int64_t i = 1; i <= 8 && !behind; i++) {
            // the recording's frame at the clock's i-th next frame (B's follows K)
            const int64_t f = std::clamp<int64_t>(v == 0 ? position_ + i : t.b_frame(position_ + i), 0, std::max<int64_t>(frames - 1, 0));
            behind = frames > 0 && !s->Ready(f);
        }
        const double fps = behind ? s->Stats().decode_fps : 0.0;
        if (fps > 0.0)
            rate = rate == 0.0 ? fps : std::min(rate, fps);
    }
    return rate;
}

void Player::SetInterest() {
    const Timeline& t = clock_.timeline();
    const int64_t position = clock_.position();
    const int64_t ahead = std::max<int64_t>(2, (int64_t)std::ceil(config_.ahead_seconds * t.fps * std::max(clock_.rate(), 1.0)));
    const int direction = clock_.playing() ? 1 : 0;
    position_ = position;
    for (int v = 0; v < 2; v++) {
        const int64_t frames = v == 0 ? t.frames_a : t.frames_b;
        if (frames <= 0)
            continue;
        // Outside its range a recording keeps its nearest end ready, so it is there when the clock reaches it. B's frame
        // at the clock is n - K(n) and its window is what the pairs of the next frames need: it follows K (a pair that
        // repeats or skips a frame, a jump) and stops at a jump too long to decode through.
        int64_t head = std::clamp<int64_t>(v == 0 ? position : t.b_frame(position), 0, frames - 1);
        int64_t first = std::max<int64_t>(0, head - config_.behind_frames), last = std::min(frames - 1, head + ahead);
        if (v == 1 && !t.offsets().is_constant()) {
            // the window moves across a long jump only when the pair at the clock is on screen (nothing of the near side is
            // needed then; a seek onto the last frame before the jump still needs it)
            const Timeline::FrameRange r = t.b_range(position - config_.behind_frames, position, position + ahead,
                                                     config_.max_gap_frames, clock_.shown() == position ? 1 : -1);
            if (r.last >= r.first) {
                first = r.first;
                last = r.last;
                head = std::clamp(r.head, first, last);
            }
        }
        source_[v]->SetInterest(first, last, head, direction);
    }
}

Player::Tick Player::Update(double now) {
    SetInterest();
    const Lockstep::Decision d = clock_.Tick(now, *this, capability());
    if (d.changed) {
        auto pair = std::make_shared<Pair>();
        pair->n = d.pair;
        pair->source[0] = source_[0];
        pair->source[1] = source_[1];
        bool ok = true;
        if (d.has_a) {
            ok = source_[0]->Acquire(d.pair, pair->a);
            pair->has_a = ok;
        }
        if (ok && d.has_b) {
            ok = source_[1]->Acquire(d.b_frame, pair->b);
            pair->has_b = ok;
        }
        if (ok && ((d.has_a && pair->a.index != d.pair) || (d.has_b && pair->b.index != d.b_frame))) {
            mismatched_++;  // never shown
            ok = false;
        }
        if (ok) {
            pair_ = pair;
        } else {
            acquire_failures_++;
            clock_.Invalidate();
        }
    }
    Tick out;
    out.pair = pair_;
    out.changed = d.changed && pair_ && pair_->n == d.pair;
    out.buffering = d.buffering;
    out.decode_limited = clock_.decode_limited();
    out.rate = clock_.rate();
    return out;
}

}  // namespace renderer::clips
