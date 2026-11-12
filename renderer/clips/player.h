// Two video sources under one Lockstep clock: the pair on screen, the decode interest of each source, and the counters
// the check prints. No GPU or OS code of its own: the frames are the sources' (video_source.h).

#pragma once

#include <memory>

#include "renderer/clips/lockstep.h"
#include "renderer/clips/video_source.h"

namespace renderer::clips {

// A pair of pinned frames, one per recording (or one where only one exists). The frames are released when the last
// reference goes: the viewer keeps a pair until the GPU frames that used it have retired.
struct Pair {
    int64_t n = NO_PAIR;  // A's frame number; B's is n - K
    bool has_a = false, has_b = false;
    FrameRef a, b;
    VideoSource* source[2] = {nullptr, nullptr};
    Pair() = default;
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;
    ~Pair() {
        if (has_a && source[0])
            source[0]->Release(a);
        if (has_b && source[1])
            source[1]->Release(b);
    }
};

struct PlayerConfig {
    double ahead_seconds = 0.75;  // decode ahead of the position, at the play rate (at least 1x)
    int64_t behind_frames = 120;  // frames kept behind it (the source's memory limit keeps them)
    int64_t max_gap_frames = 120;  // a jump of K beyond this is not decoded through: B's decoder restarts at it
};

class Player : public Availability {
public:
    // The sources outlive the player. offsets: K(n), a constant or the project's offset map (offset_map.h).
    Player(VideoSource* a, VideoSource* b, const OffsetMap& offsets, PlayerConfig config = {});

    Lockstep& clock() { return clock_; }
    const Lockstep& clock() const { return clock_; }

    struct Tick {
        std::shared_ptr<const Pair> pair;  // the pair on screen (null: none yet)
        bool changed = false;              // new since the previous tick
        bool buffering = false;
        bool decode_limited = false;
        double rate = 1.0;
    };
    // Once per displayed frame, render thread: sets the sources' interest, moves the clock, takes the new pair.
    Tick Update(double now);

    // A change of offset (a constant or a map): the clock keeps its position.
    void SetOffsets(const OffsetMap& offsets);
    void SetOffset(int64_t k) { SetOffsets(OffsetMap(k)); }

    uint64_t mismatched() const { return mismatched_; }
    uint64_t acquire_failures() const { return acquire_failures_; }
    // Decoded pairs per second the decoders sustain, from the sources that are behind (the next frames not ready: then their
    // decode rate is what they can do, not what playback asks); the slowest of them. 0: none is behind, so unknown.
    double capability() const;

    bool Ready(int video, int64_t frame) const override { return source_[video]->Ready(frame); }

private:
    void SetInterest();

    VideoSource* source_[2];
    int64_t position_ = 0;  // the clock's frame (A's numbering) when the interest was last set
    PlayerConfig config_;
    Lockstep clock_;
    std::shared_ptr<const Pair> pair_;
    uint64_t mismatched_ = 0, acquire_failures_ = 0;
};

}  // namespace renderer::clips
