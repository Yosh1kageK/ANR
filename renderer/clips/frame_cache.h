// The bookkeeping of a pool of slots that hold decoded frames around the playhead: which frame is in which slot, which
// slots are loading, ready or pinned, which to evict for a new frame, and which frame the decoder needs next. Pure (no
// GPU, no OS); the FFmpeg source keeps its frames in GPU textures, one per slot. Tested by frame_cache_test.cpp.
//
// The wanted window is [first, last] around the playhead. A new frame takes a free slot; else the unpinned ready frame
// farthest from the playhead outside the window; else the farthest one behind the playhead (frames ahead of it are
// what playback needs, so they are never dropped for a frame further ahead). Loading and pinned slots are never taken.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace renderer::clips {

struct FrameWindow {
    int64_t first = 0, last = -1, playhead = 0;
    bool contains(int64_t f) const { return f >= first && f <= last; }
};

class SlotCache {
public:
    enum class State : uint8_t { FREE, LOADING, READY };

    explicit SlotCache(int capacity = 0) { Reset(capacity); }
    void Reset(int capacity) {
        slots_.assign((size_t)capacity, Slot());
        index_.clear();
    }

    int capacity() const { return (int)slots_.size(); }
    State state(int slot) const { return slots_[(size_t)slot].state; }
    int64_t frame_of(int slot) const { return slots_[(size_t)slot].frame; }
    int pins(int slot) const { return slots_[(size_t)slot].pins; }

    // The slot holding `frame` in any state, or -1.
    int Find(int64_t frame) const {
        auto it = index_.find(frame);
        return it == index_.end() ? -1 : it->second;
    }
    bool IsReady(int64_t frame) const {
        const int s = Find(frame);
        return s >= 0 && slots_[(size_t)s].state == State::READY;
    }
    bool Present(int64_t frame) const { return Find(frame) >= 0; }

    // A slot to load `frame` into (marked LOADING), evicting if needed; -1 when none may be taken now.
    int Reserve(int64_t frame, const FrameWindow& window) {
        if (Find(frame) >= 0)
            return -1;
        int best = -1;
        int64_t best_distance = -1;
        bool best_outside = false;
        for (int i = 0; i < capacity(); i++) {
            const Slot& s = slots_[(size_t)i];
            if (s.state == State::FREE) {
                best = i;
                best_outside = true;
                best_distance = INT64_MAX;
                break;
            }
            if (s.state != State::READY || s.pins > 0)
                continue;
            const bool outside = !window.contains(s.frame);
            const bool behind = s.frame < window.playhead && s.frame < frame;
            if (!outside && !behind)
                continue;
            const int64_t distance = std::llabs(s.frame - window.playhead);
            // Outside the window first, then behind it; the farthest first.
            if (best < 0 || (outside && !best_outside) || (outside == best_outside && distance > best_distance)) {
                best = i;
                best_distance = distance;
                best_outside = outside;
            }
        }
        if (best < 0)
            return -1;
        Slot& s = slots_[(size_t)best];
        if (s.state != State::FREE)
            index_.erase(s.frame);
        s.frame = frame;
        s.state = State::LOADING;
        s.pins = 0;
        index_[frame] = best;
        return best;
    }

    void MarkReady(int slot) { slots_[(size_t)slot].state = State::READY; }
    // A load that failed or was dropped.
    void Free(int slot) {
        Slot& s = slots_[(size_t)slot];
        if (s.state != State::FREE)
            index_.erase(s.frame);
        s = Slot();
    }
    void Clear() {
        for (int i = 0; i < capacity(); i++)
            if (slots_[(size_t)i].state == State::READY && slots_[(size_t)i].pins == 0)
                Free(i);
    }

    // Pins a ready frame; false when it is not ready.
    bool Pin(int64_t frame, int& slot) {
        slot = Find(frame);
        if (slot < 0 || slots_[(size_t)slot].state != State::READY)
            return false;
        slots_[(size_t)slot].pins++;
        return true;
    }
    void Unpin(int slot) {
        if (slots_[(size_t)slot].pins > 0)
            slots_[(size_t)slot].pins--;
    }

    int ReadyCount() const {
        int n = 0;
        for (const Slot& s : slots_)
            n += s.state == State::READY;
        return n;
    }
    int LoadingCount() const {
        int n = 0;
        for (const Slot& s : slots_)
            n += s.state == State::LOADING;
        return n;
    }

    // The first frame of the window the cache lacks: from the playhead forward, else behind it nearest first; -1 when
    // the window is complete.
    int64_t NextMissing(const FrameWindow& window) const {
        for (int64_t f = std::max(window.playhead, window.first); f <= window.last; f++)
            if (!Present(f))
                return f;
        for (int64_t f = std::min(window.playhead - 1, window.last); f >= window.first; f--)
            if (!Present(f))
                return f;
        return -1;
    }

private:
    struct Slot {
        int64_t frame = -1;
        State state = State::FREE;
        int pins = 0;
    };
    std::vector<Slot> slots_;
    std::unordered_map<int64_t, int> index_;
};

// What the decoder does next: keep reading frames from where its stream is, restart it at a frame, or wait.
struct DecodePlan {
    enum class Action { IDLE, CONTINUE, RESTART } action = Action::IDLE;
    int64_t restart_at = 0;  // RESTART: the first frame of the new stream
};

// A stream that delivers frames in order from `next` (-1: none running). `need` is the first missing frame of the
// window. Continue when the stream will reach it soon; else restart a little before it (preroll frames: they cost a
// decode each but make a step back right after a seek instant). skip_limit: the most frames worth decoding through
// instead of seeking (a keyframe gap's worth).
inline DecodePlan PlanDecode(int64_t next, int64_t need, int64_t frames, int64_t preroll, int64_t skip_limit) {
    DecodePlan plan;
    if (need < 0 || need >= frames)
        return plan;
    if (next >= 0 && next <= need && need - next <= skip_limit) {
        plan.action = DecodePlan::Action::CONTINUE;
        return plan;
    }
    plan.action = DecodePlan::Action::RESTART;
    plan.restart_at = std::max<int64_t>(0, need - preroll);
    return plan;
}

}  // namespace renderer::clips
