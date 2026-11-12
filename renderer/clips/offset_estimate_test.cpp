// Start sync detection and drift tracking on synthetic recordings: A shows scene n at frame n (new at every frame, smooth
// in space, a static HUD box in the corner); B shows the scenes of A as a model says (a constant offset, a drift, a
// pause), so what the tracker should find is known.
#include "renderer/clips/offset_estimate.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <string>

#include "renderer/clips/check.h"

using namespace renderer::clips;

namespace {

uint64_t splitmix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

double unit(uint64_t x) {
    return (double)(splitmix(x) >> 11) / 9007199254740992.0;
}

// The scene of one id: a few low-frequency waves with the id's own random phases, unrelated from one id to the next, so
// the match is sharp in time at any rate.
double scene(double x, double y, int64_t id) {
    const uint64_t frame = (uint64_t)id + (1ull << 40);
    double sum = 0.0;
    for (int k = 0; k < 6; k++) {
        const double fx = 1.0 + 4.0 * unit(frame * 31 + k * 7 + 1), fy = 1.0 + 4.0 * unit(frame * 31 + k * 7 + 2);
        const double phase = 6.283185307179586 * unit(frame * 31 + k * 7 + 3);
        sum += std::sin(6.283185307179586 * (fx * x + fy * y) + phase);
    }
    return sum;
}

// One 128x72 grey frame of a scene: the scene on a 32x18 lattice, interpolated, with a static HUD box (a pattern that is the
// same in every frame of both recordings) in the top-left corner: x < 40, y < 12 (10 x 3 cells).
void render(int64_t id, uint8_t* out, bool hud = true) {
    float lattice[19][33];
    for (int ly = 0; ly < 19; ly++)
        for (int lx = 0; lx < 33; lx++)
            lattice[ly][lx] = (float)scene(lx / 32.0, ly / 18.0, id);
    for (int y = 0; y < FEED_H; y++) {
        const int ly = y / 4;
        const float wy = (y % 4) / 4.0f;
        for (int x = 0; x < FEED_W; x++) {
            const int lx = x / 4;
            const float wx = (x % 4) / 4.0f;
            const float v = (1 - wy) * ((1 - wx) * lattice[ly][lx] + wx * lattice[ly][lx + 1]) +
                            wy * ((1 - wx) * lattice[ly + 1][lx] + wx * lattice[ly + 1][lx + 1]);
            out[y * FEED_W + x] = (uint8_t)std::lround(std::clamp(128.0f + 20.0f * v, 0.0f, 255.0f));
        }
    }
    if (hud)
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 40; x++)
                out[y * FEED_W + x] = (uint8_t)(20 + ((x / 5 + y / 3) % 2) * 200);
}

// A has `frames_a` frames, scene n at frame n; B has `frames_b` frames, frame m showing the scene b_scene(m).
class SyntheticFeed : public FrameFeed {
public:
    SyntheticFeed(int64_t frames_a, int64_t frames_b, std::function<int64_t(int64_t)> b_scene)
        : frames_a_(frames_a), frames_b_(frames_b), b_scene_(std::move(b_scene)) {}
    bool Frames(int video, int64_t first, int64_t count, int w, int h, const Sink& sink) override {
        CHECK_EQ(w, FEED_W);
        CHECK_EQ(h, FEED_H);
        const int64_t frames = video == 0 ? frames_a_ : frames_b_;
        const int64_t last = count < 0 ? frames : std::min(frames, first + count);
        std::vector<uint8_t> grey((size_t)FEED_W * FEED_H);
        for (int64_t i = first; i < last; i++) {
            render(video == 0 ? i : b_scene_(i), grey.data());
            if (!sink(i, grey.data()))
                return false;
        }
        calls++;
        return last > first;
    }
    std::atomic<int> calls{0};

private:
    int64_t frames_a_, frames_b_;
    std::function<int64_t(int64_t)> b_scene_;
};

// B is A delayed: A frame n is B frame n - k.
SyntheticFeed constant_feed(int64_t frames_a, int64_t frames_b, int64_t k) {
    return SyntheticFeed(frames_a, frames_b, [k](int64_t m) { return m + k; });
}

VideoFeatures features_of(FrameFeed& feed, int video) {
    VideoFeatures f;
    CHECK(feed.Frames(video, 0, -1, FEED_W, FEED_H, [&](int64_t, const uint8_t* g) {
        f.Add(g);
        return true;
    }));
    return f;
}

void descriptors_and_mask() {
    // a static box is found in the recording, and nothing else; either recording's flags count
    SyntheticFeed feed = constant_feed(200, 200, 0);
    const VideoFeatures f = features_of(feed, 0);
    CHECK_EQ((int)f.count, 200);
    const std::vector<uint8_t> flags = StaticCells(f);
    int inside = 0, outside = 0;
    for (int c = 0; c < VideoFeatures::CELLS; c++) {
        const bool box = c % VideoFeatures::CELLS_X < 10 && c / VideoFeatures::CELLS_X < 3;
        (box ? inside : outside) += flags[(size_t)c];
    }
    CHECK_EQ(inside, 30);
    CHECK_EQ(outside, 0);
    double fraction = 0.0;
    const std::vector<uint8_t> mask = CombineMasks(flags, std::vector<uint8_t>(VideoFeatures::CELLS, 0), &fraction);
    CHECK_NEAR(fraction, 30.0 / VideoFeatures::CELLS, 1e-12);
    // a recording too short to judge, and one that is all still, mask nothing
    VideoFeatures few;
    std::vector<uint8_t> grey((size_t)FEED_W * FEED_H, 100);
    for (int i = 0; i < 10; i++)
        few.Add(grey.data());
    const std::vector<uint8_t> few_flags = StaticCells(few);
    CHECK_EQ((int)std::count(few_flags.begin(), few_flags.end(), 1), 0);
    VideoFeatures still;
    for (int i = 0; i < 60; i++)
        still.Add(grey.data());
    double still_fraction = 1.0;
    CombineMasks(StaticCells(still), StaticCells(still), &still_fraction);
    CHECK_NEAR(still_fraction, 0.0, 1e-12);
    // descriptors: unit rows, the masked cells out of them, and a gain and an offset of the picture change nothing
    const Descriptors d = BuildDescriptors(f, 0, 3, 1, mask);
    CHECK_EQ(d.count, 3);
    for (int i = 0; i < 3; i++)
        CHECK_NEAR(std::sqrt(std::inner_product(d.row(i), d.row(i) + d.dim, d.row(i), 0.0)), 1.0, 1e-4);
    for (int c = 0; c < VideoFeatures::CELLS; c++)
        if (mask[(size_t)c])
            CHECK_NEAR(d.row(0)[c], 0.0, 0.0);
    CHECK_NEAR(std::inner_product(d.row(0), d.row(0) + d.dim, d.row(1), 0.0), 0.0, 0.15);  // two scenes: unrelated
    VideoFeatures dim_copy;
    std::vector<uint8_t> frame((size_t)FEED_W * FEED_H);
    render(0, frame.data());
    for (uint8_t& v : frame)
        v = (uint8_t)std::lround(0.7 * v + 20.0);  // the exposure changed
    dim_copy.Add(frame.data());
    const Descriptors e = BuildDescriptors(dim_copy, 0, 1, 1, mask);
    CHECK(std::inner_product(d.row(0), d.row(0) + d.dim, e.row(0), 0.0) > 0.97);
    // frames the recording does not hold are left out
    CHECK_EQ(BuildDescriptors(f, 198, 5, 1, mask).count, 2);
}

void coarse_and_window_by_hand() {
    SyntheticFeed feed = constant_feed(2400, 2100, 408);  // A = B + 6.8 s
    const VideoFeatures a = features_of(feed, 0), b = features_of(feed, 1);
    const std::vector<uint8_t> mask = CombineMasks(StaticCells(a), StaticCells(b));
    const Descriptors ca = BuildDescriptors(a, 0, 400, 6, mask), cb = BuildDescriptors(b, 0, 350, 6, mask);
    SyncCoarse c;
    CHECK(CoarseOffset(ca, cb, 10.0, c));
    CHECK_NEAR(c.offset, 6.8, 1e-9);
    CHECK(c.similarity > 0.9);
    CHECK(c.next_best < 0.5);
    CHECK(c.z > 5.0);
    // a window of A at frame 1200 against B within +-36 frames of a wrong centre
    for (double centre : {408.0, 380.0, 435.0}) {
        const SyncWindow w = TrackWindow(a, b, mask, 1200, 600, centre, 36, 60.0);
        CHECK_NEAR(w.offset * 60.0, 408.0, 0.5);
        CHECK_NEAR(w.a_time, 1500.0 / 60.0, 1e-9);  // the window's middle
        CHECK(w.used && WindowSharp(w));
        CHECK(w.confidence > 0.8);
        CHECK(w.width_frames <= 3);
    }
    // a window whose peak is outside the lags found nothing sharp
    const SyncWindow off = TrackWindow(a, b, mask, 1200, 600, 300.0, 36, 60.0);
    CHECK(!off.used);
    // two recordings of 5 s share nothing coarse can use
    SyncCoarse none;
    CHECK(!CoarseOffset(BuildDescriptors(a, 0, 40, 6, mask), BuildDescriptors(b, 0, 40, 6, mask), 10.0, none));
}

// The robust fit on window offsets: a slope, one jump found, a lone outlier rejected, a step of a frame left alone.
void fit_slope_and_jump() {
    auto noise = [](int i) { return 0.3 * (2.0 * unit((uint64_t)i * 17 + 5) - 1.0); };
    auto windows_of = [&](double slope, double jump_at, double jump_by, double step_to_jump) {
        std::vector<SyncWindow> ws;
        for (int i = 0; i < 22; i++) {
            const double t = 300.0 + 240.0 * i;  // A frames, the window middle
            double k = 400.0 + slope * t + noise(i);
            if (t > jump_at)
                k += jump_by;
            (void)step_to_jump;
            SyncWindow w;
            w.a_time = t / 60.0;
            w.offset = k / 60.0;
            w.contrast = 0.5;
            w.width_frames = 3;
            w.used = true;
            ws.push_back(w);
        }
        return ws;
    };
    // a slope of 0.004 and a pause at 2700: K drops 20
    std::vector<SyncWindow> ws = windows_of(0.004, 2700.0, -20.0, 0.0);
    ws[4].offset += 9.0 / 60.0;  // a window that measured a wrong peak
    ws[9].used = false;           // and one the tracker did not trust
    TrackFit fit = FitOffsets(ws, 60.0);
    CHECK(fit.ok);
    CHECK_NEAR(fit.slope, 0.004, 0.0006);
    CHECK_EQ((int)fit.splits.size(), 1);
    if (fit.splits.empty())
        return;
    CHECK_EQ((int)fit.intercepts.size(), 2);
    CHECK_NEAR(fit.intercepts[1] - fit.intercepts[0], -20.0, 0.6);
    CHECK(fit.splits[0].left <= 2700.0 && fit.splits[0].right > 2700.0 && fit.splits[0].right - fit.splits[0].left <= 240.0 + 1e-9);
    CHECK(!ws[4].used);  // the outlier is out; the one that was out stays out
    CHECK(!ws[9].used);
    CHECK_EQ(fit.used, 20);
    CHECK_NEAR(ws[4].residual_frames, 9.0, 0.6);
    CHECK_NEAR(ws[3].residual_frames, 0.0, 0.5);
    // no jump: one segment
    ws = windows_of(-0.002, 1e9, 0.0, 0.0);
    fit = FitOffsets(ws, 60.0);
    CHECK(fit.ok && fit.splits.empty());
    CHECK_NEAR(fit.slope, -0.002, 0.0006);
    CHECK_EQ(fit.used, 22);
    // a step of one frame is below what counts as a jump, and so is a single window off by three
    ws = windows_of(0.0, 2000.0, 1.0, 0.0);
    fit = FitOffsets(ws, 60.0);
    CHECK(fit.ok && fit.splits.empty());
    // a jump with one window on a side is not sustained: the window is an outlier
    ws = windows_of(0.0, 300.0 + 240.0 * 20.5, 8.0, 0.0);
    fit = FitOffsets(ws, 60.0);
    CHECK(fit.ok && fit.splits.empty());
    CHECK(!ws[21].used);
    // fewer than two windows: no fit
    ws = windows_of(0.0, 1e9, 0.0, 0.0);
    for (size_t i = 1; i < ws.size(); i++)
        ws[i].used = false;
    CHECK(!FitOffsets(ws, 60.0).ok);
}

void whole_estimate_constant() {
    SyntheticFeed feed = constant_feed(2400, 2100, 408);  // A = B + 6.8 s
    SyncResult r;
    std::vector<std::string> log;
    double last_progress = -1.0;
    const bool ok = EstimateOffset(feed, 40.0, 35.0, 60.0, r, [&](const std::string& s) { log.push_back(s); },
                                   [&](double p) {
                                       CHECK(p >= last_progress - 1e-9);
                                       last_progress = p;
                                   });
    CHECK(ok);
    CHECK_NEAR(last_progress, 1.0, 1e-12);
    CHECK_EQ(feed.calls.load(), 2);  // each recording is decoded once
    CHECK_EQ(r.offset_frames, 408);
    CHECK(r.quality == "fine");
    CHECK(r.spread_seconds.has_value());
    CHECK(*r.spread_seconds * 60.0 <= 1.0);
    CHECK_NEAR(r.coarse.offset, 6.8, 1e-9);
    CHECK(r.coarse.similarity > 0.9 && r.coarse.next_best < 0.6);
    CHECK(!r.detected_at.empty());
    CHECK((int)log.size() == 3);  // decoded, coarse, tracking
    CHECK_NEAR(r.offset, 6.8, 0.25 / 60.0);
    // the tracking: no drift, no map, the HUD box masked
    CHECK(r.tracking.present && !r.tracking.significant && r.tracking.map.empty() && r.tracking.jumps.empty());
    CHECK(r.tracking.windows_used >= 5 && r.tracking.windows_rejected == 0);
    CHECK(std::fabs(r.tracking.drift_frames) < 0.5);
    CHECK(r.tracking.confidence > 0.8);
    CHECK_NEAR(r.tracking.mask_fraction, 30.0 / 576.0, 1e-9);
    for (const SyncWindow& w : r.windows)
        CHECK(w.used && std::fabs(w.residual_frames) < 0.5);
}

void negative_and_short() {
    // B started first: A = B - 5.0 s
    SyntheticFeed feed = constant_feed(2400, 2700, -300);
    SyncResult r;
    CHECK(EstimateOffset(feed, 40.0, 45.0, 60.0, r));
    CHECK_EQ(r.offset_frames, -300);
    CHECK_NEAR(r.coarse.offset, -5.0, 1e-9);
    CHECK(r.quality == "fine");
    // recordings that overlap by 10.5 s have the coarse stage but no windows: the coarse offset stands, flagged
    SyntheticFeed short_feed = constant_feed(1080, 1200, 450);
    SyncResult s;
    CHECK(EstimateOffset(short_feed, 18.0, 20.0, 60.0, s));
    CHECK(s.quality == "coarse");
    CHECK(!s.spread_seconds.has_value());
    CHECK(s.windows.empty());
    CHECK_NEAR(s.offset, 7.5, 1e-9);
    CHECK_EQ(s.offset_frames, 450);
    // less than 10 s in common: no result
    SyntheticFeed none = constant_feed(540, 540, 300);
    SyncResult n;
    std::string line;
    CHECK(!EstimateOffset(none, 9.0, 9.0, 60.0, n, [&](const std::string& text) { line = text; }));
    CHECK(line.find("less than 10 s") != std::string::npos);
}

// B runs 2000 ppm slow (K rises 0.002 per A frame, from 300) and pauses for 30 frames (0.5 s) when A is at frame 2700: K
// falls by 30 there. The tracker finds the slope and the jump, and the map it makes gives K(n) to a frame and a half.
void whole_estimate_drift_and_pause() {
    const double k0 = 300.0, s = 0.002;
    const int64_t pause_at = 2700, pause = 30, frames_a = 5400;
    const int64_t m_pause = (int64_t)std::llround((double)pause_at * (1.0 - s) - k0);  // B's frame when it stops
    auto b_scene = [=](int64_t m) -> int64_t {
        if (m < m_pause)
            return (int64_t)std::llround(((double)m + k0) / (1.0 - s));
        if (m < m_pause + pause)
            return pause_at;  // frozen
        return (int64_t)std::llround(((double)(m - pause) + k0) / (1.0 - s));
    };
    const int64_t frames_b = (int64_t)std::llround((double)(frames_a - 40) * (1.0 - s) - k0 + pause);
    SyntheticFeed feed(frames_a, frames_b, b_scene);
    SyncResult r;
    CHECK(EstimateOffset(feed, 90.0, (double)frames_b / 60.0, 60.0, r));
    const SyncTracking& t = r.tracking;
    CHECK(t.present && t.significant);
    CHECK(r.quality == "jumps");
    CHECK_NEAR(t.slope_ppm, 2000.0, 400.0);
    CHECK_EQ((int)t.jumps.size(), 1);
    CHECK_NEAR(t.jumps[0].frames, -30.0, 2.0);
    CHECK(std::fabs(t.jumps[0].a_time * 60.0 - (double)pause_at) < 45.0);  // somewhere in the 30 frames of the pause
    CHECK_EQ((int)t.map.size(), 4);  // the start, the two points of the jump, the end
    CHECK_EQ(t.map[2].a - t.map[1].a, 1);
    const OffsetMap map(t.map, r.offset_frames);
    CHECK_EQ(r.offset_frames, map.first_k());
    for (int64_t n : {700, 1500, 2400, 3300, 4000, 5000}) {
        const double truth = k0 + s * (double)n - (n > pause_at + 45 ? (double)pause : 0.0);
        if (n > pause_at - 45 && n < pause_at + 45)
            continue;
        CHECK_NEAR((double)map.at(n), truth, 1.5);
    }
    CHECK(t.windows_used >= 15);
    CHECK(t.confidence > 0.7);
}

void local_window() {
    SyntheticFeed feed = constant_feed(2400, 2100, 408);
    SyncWindow w;
    // the project says 6.9 s (6 frames off): the window around the playhead finds 6.8
    CHECK(LocalOffset(feed, 40.0, 35.0, 60.0, 25.0, 6.9, w));
    CHECK_NEAR(w.offset, 6.8, 1.0 / 120.0);
    CHECK(WindowSharp(w) && w.used);
    CHECK_NEAR(w.a_time, 25.0, 5.1);
    // too close to the start of B's range: no window
    CHECK(!LocalOffset(feed, 40.0, 35.0, 60.0, 3.0, 6.9, w) || std::fabs(w.a_time - 3.0) > 4.0);
    // frames the feed does not have: false
    SyntheticFeed short_b = constant_feed(2400, 100, 408);
    CHECK(!LocalOffset(short_b, 40.0, 35.0, 60.0, 25.0, 6.9, w));
}

}  // namespace

int main() {
    descriptors_and_mask();
    coarse_and_window_by_hand();
    fit_slope_and_jump();
    whole_estimate_constant();
    negative_and_short();
    whole_estimate_drift_and_pause();
    local_window();
    return check_result("offset_estimate");
}
