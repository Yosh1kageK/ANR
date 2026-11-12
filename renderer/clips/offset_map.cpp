#include "renderer/clips/offset_map.h"

#include <algorithm>
#include <cstdlib>

namespace renderer::clips {

OffsetMap::OffsetMap(std::vector<OffsetPoint> points, int64_t fallback) : points_(std::move(points)), constant_(fallback) {
    std::stable_sort(points_.begin(), points_.end(), [](const OffsetPoint& x, const OffsetPoint& y) { return x.a < y.a; });
    if (!points_.empty())
        constant_ = RoundK(points_.front().k);
}

double OffsetMap::exact(double n) const {
    if (points_.empty())
        return (double)constant_;
    if (n <= (double)points_.front().a)
        return points_.front().k;
    if (n >= (double)points_.back().a)
        return points_.back().k;
    // the last point at or before n (of equal frames, the later one)
    const auto next = std::upper_bound(points_.begin(), points_.end(), n,
                                       [](double v, const OffsetPoint& p) { return v < (double)p.a; });
    const OffsetPoint& hi = *next;
    const OffsetPoint& lo = *(next - 1);
    if (hi.a == lo.a)
        return hi.k;
    return lo.k + (hi.k - lo.k) * (n - (double)lo.a) / (double)(hi.a - lo.a);
}

int64_t OffsetMap::first_k() const {
    return points_.empty() ? constant_ : RoundK(points_.front().k);
}

int64_t OffsetMap::min_k() const {
    if (points_.empty())
        return constant_;
    double k = points_.front().k;
    for (const OffsetPoint& p : points_)
        k = std::min(k, p.k);
    return RoundK(k);
}

int64_t OffsetMap::max_k() const {
    if (points_.empty())
        return constant_;
    double k = points_.front().k;
    for (const OffsetPoint& p : points_)
        k = std::max(k, p.k);
    return RoundK(k);
}

double OffsetMap::slope_at(int64_t n) const {
    if (points_.size() < 2 || n < points_.front().a || n >= points_.back().a)
        return 0.0;
    const auto next = std::upper_bound(points_.begin(), points_.end(), n, [](int64_t v, const OffsetPoint& p) { return v < p.a; });
    const OffsetPoint& hi = *next;
    const OffsetPoint& lo = *(next - 1);
    if (hi.a - lo.a <= 2)
        return 0.0;
    return (hi.k - lo.k) / (double)(hi.a - lo.a);
}

std::vector<int64_t> OffsetMap::jumps(double frames) const {
    std::vector<int64_t> out;
    for (size_t i = 1; i < points_.size(); i++)
        if (points_[i].a - points_[i - 1].a <= 2 && std::abs(points_[i].k - points_[i - 1].k) >= frames)
            out.push_back(points_[i].a);
    return out;
}

int OffsetMap::Nearest(int64_t n) const {
    int best = -1;
    int64_t distance = 0;
    for (size_t i = 0; i < points_.size(); i++) {
        const int64_t d = std::llabs(points_[i].a - n);
        if (best < 0 || d < distance) {
            best = (int)i;
            distance = d;
        }
    }
    return best;
}

void OffsetMap::Add(int64_t n, double k) {
    for (OffsetPoint& p : points_)
        if (p.a == n) {
            p.k = k;
            constant_ = RoundK(points_.front().k);
            return;
        }
    const auto at = std::upper_bound(points_.begin(), points_.end(), n, [](int64_t v, const OffsetPoint& p) { return v < p.a; });
    points_.insert(at, OffsetPoint{n, k});
    constant_ = RoundK(points_.front().k);
}

bool OffsetMap::RemoveNearest(int64_t n) {
    const int i = Nearest(n);
    if (i < 0)
        return false;
    points_.erase(points_.begin() + i);
    if (!points_.empty())
        constant_ = RoundK(points_.front().k);
    return true;
}

void OffsetMap::Clear() {
    // the constant stays what K was at the first point: a project that drops its anchors keeps its offset_frames
    points_.clear();
}

void OffsetMap::Nudge(int64_t n, int64_t delta, int64_t near_frames) {
    const int i = Nearest(n);
    if (i >= 0 && std::llabs(points_[(size_t)i].a - n) <= near_frames) {
        points_[(size_t)i].k += (double)delta;
        constant_ = RoundK(points_.front().k);
        return;
    }
    if (points_.empty()) {
        // the first anchor: K(n) + delta here, and the constant before and after it
        Add(n, (double)(constant_ + delta));
        return;
    }
    Add(n, (double)(at(n) + delta));
}

OffsetMap OffsetMap::Swapped() const {
    if (points_.empty())
        return OffsetMap(-constant_);
    std::vector<OffsetPoint> swapped;
    swapped.reserve(points_.size());
    for (const OffsetPoint& p : points_)
        swapped.push_back({(int64_t)std::llround((double)p.a - p.k), -p.k});
    return OffsetMap(std::move(swapped), -constant_);
}

}  // namespace renderer::clips
