// The offset between two recordings as a function of A's frame: K(n), so that frame n of A shows the moment of frame
// n - K(n) of B. Recordings started by hand run on different clocks and pause or stutter, so one constant K is right only
// near one place; the map holds control points {a: A frame, k: K in frames}, sorted by a. K(n) is the linear
// interpolation between the control points, rounded to the nearest whole frame (halves up), constant before the first
// point and after the last; a jump (a pause or a stutter) is two points one frame apart. Without points the map is the
// constant it was made with, which is the project's `offset_frames` and the behaviour before maps existed.
//
// Where K changes by 1 between two frames of A, B repeats a frame (K up) or skips one (K down); a jump seeks B. Pure
// (no GPU, no OS), tested by lockstep_test.cpp and project_test.cpp.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace renderer::clips {

struct OffsetPoint {
    int64_t a = 0;   // A frame number
    double k = 0.0;  // K at that frame, in frames (the interpolation may leave it fractional)
};

// A whole frame: the nearest, halves up (so K and -K of the same moment differ only at exact halves).
inline int64_t RoundK(double k) {
    return (int64_t)std::floor(k + 0.5);
}

class OffsetMap {
public:
    OffsetMap(int64_t constant = 0) : constant_(constant) {}  // implicit: a plain offset is a map
    // Points in any order (sorted by A frame, equal frames keep their order); `fallback` is the constant when there are none.
    explicit OffsetMap(std::vector<OffsetPoint> points, int64_t fallback = 0);

    bool is_constant() const { return points_.empty(); }
    const std::vector<OffsetPoint>& points() const { return points_; }
    int64_t constant() const { return constant_; }

    // K at A frame n, and K before rounding (n may be fractional: the plot and the local slope use it).
    int64_t at(int64_t n) const { return RoundK(exact((double)n)); }
    double exact(double n) const;
    // K at the first control point, rounded (the constant without points): the project's `offset_frames`.
    int64_t first_k() const;
    // The lowest and highest K of the whole map, rounded (the constant without points).
    int64_t min_k() const;
    int64_t max_k() const;
    // dK/dn (frames per frame) of the segment that holds frame n; 0 outside the points, in a jump (a segment of at most
    // two frames) and without points.
    double slope_at(int64_t n) const;

    // The A frames that start a jump: where K changes by at least `frames` between two points at most two frames apart.
    std::vector<int64_t> jumps(double frames = 1.5) const;

    // Anchors (the viewer's edits). Each keeps the points sorted.
    void Add(int64_t n, double k);              // a point at A frame n; one already there changes its K
    bool RemoveNearest(int64_t n);              // false: no points
    void Clear();                               // back to the constant
    // A nudge of `delta` frames at A frame n: the nearest point within `near_frames` of n moves by delta; with none that
    // close a point is made at n with K(n) + delta.
    void Nudge(int64_t n, int64_t delta, int64_t near_frames);
    // The index of the point nearest to n, -1 without points.
    int Nearest(int64_t n) const;

    // A and B swapped: A frame n matched B frame m = n - K(n), so the new map has a point {m, -K} for each of these.
    // Swapping twice gives the points back.
    OffsetMap Swapped() const;

    // B's frame m of A frame n (n - K(n)).
    int64_t b_frame(int64_t n) const { return n - at(n); }

    friend bool operator==(const OffsetMap& x, const OffsetMap& y) {
        if (x.points_.size() != y.points_.size())
            return false;
        for (size_t i = 0; i < x.points_.size(); i++)
            if (x.points_[i].a != y.points_[i].a || x.points_[i].k != y.points_[i].k)
                return false;
        return x.points_.empty() ? x.constant_ == y.constant_ : true;
    }
    friend bool operator!=(const OffsetMap& x, const OffsetMap& y) { return !(x == y); }

private:
    std::vector<OffsetPoint> points_;
    int64_t constant_ = 0;
};

}  // namespace renderer::clips
