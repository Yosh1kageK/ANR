// Start sync detection and drift tracking. Two recordings of the same run, started by hand, are offset by K frames (frame
// n of A shows the moment of frame n - K of B) and drift: different clocks and frame pacing, pauses, stutters. This finds
// K(n) (offset_map.h) from the pictures alone. It started as a port of the earlier Python version's estimate_offset (one
// constant K from a coarse search and four windows); it is now a tracker:
//
//  1. Both recordings are decoded once, at their own frame rate (frame numbers are the player's, no resampling), as grey
//     frames of 128x72, and kept as cell features: the luma of 32x18 cells and the gradient (edge) magnitude of the same
//     cells. A cell is "static" when its pixels barely change over a whole recording (a letterbox, a HUD, an overlay's
//     text box); the cells static in either recording are left out of every match (they carry no timing and add the
//     same similarity at every lag).
//  2. A frame's descriptor is the luma at two scales (32x18 and 16x9) and the gradient, each block mean-removed and
//     scaled to unit length over the cells that count, weighted 0.3, 0.3, 0.4, so the dot product of two descriptors is
//     the weighted correlation of the blocks. Exposure changes and the game's own overlay text matter less.
//  3. Coarse: the mean similarity of descriptors at about 10 fps along every diagonal with at least 10 s of overlap. It
//     gives K near the middle of the recordings to about a tenth of a second.
//  4. Dense tracking: windows of 10 s at the frame rate, about every 4 s over the whole overlap, each searched at lags
//     of +-0.6 s around the running estimate (the neighbour's result; the radius grows after a rejected window), with a
//     parabolic refinement of the peak to a fraction of a frame, and a contrast, a width and a confidence. A window is
//     used when its peak is sharp.
//  5. A robust fit (FitOffsets): Theil-Sen slope shared by all segments and a median intercept per segment, and a split
//     where a sustained residual (two or more windows on each side) is above 1.5 frames: a pause or a stutter. Windows
//     off the fit by more than that are rejected. Each jump is placed to the frame by matching the frames between the
//     two windows under both lines (RefineJump). The result is the control points of an offset map.
//
// Pure (the frames come from a FrameFeed: ffmpeg subprocesses in the viewer, synthetic ones in the tests), tested by
// offset_estimate_test.cpp.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "renderer/clips/project.h"

namespace renderer::clips {

inline constexpr int FEED_W = 128, FEED_H = 72;  // the grey frames the feed gives

class FrameFeed {
public:
    virtual ~FrameFeed() = default;
    // Grey frames of FEED_W x FEED_H (w, h say so) of video `video` (0 A, 1 B): `count` frames from frame `first` (the
    // recording's own frame numbers; count < 0: to the end), each handed to `sink` in order (the pixels are valid during
    // the call; the sink returns false to stop). False: failed, cancelled, or stopped. Called from several threads at once.
    using Sink = std::function<bool(int64_t frame, const uint8_t* grey)>;
    virtual bool Frames(int video, int64_t first, int64_t count, int w, int h, const Sink& sink) = 0;
    virtual bool Cancelled() const { return false; }
};

// One recording's frames as cell features, and the per-pixel statistics of the static mask.
struct VideoFeatures {
    static constexpr int CELLS_X = 32, CELLS_Y = 18, CELLS = CELLS_X * CELLS_Y;
    static constexpr int BYTES = 2 * CELLS;  // per frame: the luma of the cells, then their gradient magnitude

    int64_t base = 0;  // the recording's frame number of the first frame held (a slice of a recording starts later)
    int64_t count = 0;
    std::vector<uint8_t> cells;
    std::vector<double> sum, sum2;  // over the frames, per pixel of the grey frame

    void Add(const uint8_t* grey);  // one FEED_W x FEED_H frame
    // The features of the recording's frame i (base <= i < base + count).
    const uint8_t* frame(int64_t i) const { return cells.data() + (size_t)(i - base) * BYTES; }
};

// 1 where the cell is static in the recording (a share of its pixels with a standard deviation over time far below the
// typical one); CELLS flags.
std::vector<uint8_t> StaticCells(const VideoFeatures& f);
// The flags of either recording; none at all when they would cover too much of the picture (a still recording).
std::vector<uint8_t> CombineMasks(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double* fraction = nullptr);

// Rows of unit length (the dot product of two rows is their weighted correlation).
struct Descriptors {
    int count = 0, dim = 0;
    std::vector<float> data;
    const float* row(int i) const { return data.data() + (size_t)i * dim; }
};
// count rows, row i from frame first + i * step of the recording; frames outside what f holds are left out (at the end).
Descriptors BuildDescriptors(const VideoFeatures& f, int64_t first, int count, int step, const std::vector<uint8_t>& mask);

// The coarse stage on descriptors of cf frames per second (a at its own, b likewise): offset in seconds. False: no lag
// has 10 s of overlap.
bool CoarseOffset(const Descriptors& a, const Descriptors& b, double cf, SyncCoarse& out);

// One window of `len` frames of A from frame a0 against B at lags K within +-radius frames of `center` (frames).
// The result: a_time is the window's middle (seconds), offset is K / fps (seconds); `used` says it is sharp.
SyncWindow TrackWindow(const VideoFeatures& a, const VideoFeatures& b, const std::vector<uint8_t>& mask, int64_t a0, int len,
                       double center, int radius, double fps);

// A peak counts when its contrast over the curve's median is at least 0.10 and its top (the lags within 0.05 of it) is at
// most 25 frames wide. A slow scene has tops of 15 to 25 frames whose centres still agree to a frame and a half (the GTA V
// pair, calibrated on it: the old limit of 15 left most of its first minute out and the fit then reached the offset of the
// sharper windows there instead of the one the slow ones measured).
inline bool WindowSharp(const SyncWindow& w) { return w.contrast >= 0.10 && w.width_frames <= 25; }

// The robust fit of window offsets: K(n) = intercepts[segment] + slope * n with one slope.
struct TrackFit {
    bool ok = false;
    double slope = 0.0;              // frames of K per frame of A
    std::vector<double> intercepts;  // per segment, in time order
    struct Split {                   // a jump: the A frames (window middles) of the used windows either side of it
        double left = 0.0, right = 0.0;
    };
    std::vector<Split> splits;       // between segment s and s + 1
    int used = 0, rejected = 0;      // windows
    double At(size_t segment, double n) const { return intercepts[segment] + slope * n; }
};
// windows: any order, those with `used` set are fitted. Rejects (clears `used` of) windows off the fit by more than 1.5
// frames (or three times the spread of the residuals, if more), and sets every window's residual_frames against its own
// segment's line. ok is false with fewer than two windows left.
TrackFit FitOffsets(std::vector<SyncWindow>& windows, double fps);

// Where a jump between the A frames t_left and t_right happened: the frame at which the match under the line before it
// (k = left_b + slope * n) gives way to the match under the line after it, from the frames themselves.
int64_t RefineJump(const VideoFeatures& a, const VideoFeatures& b, const std::vector<uint8_t>& mask, int64_t t_left,
                   int64_t t_right, double left_b, double right_b, double slope);

using SyncLog = std::function<void(const std::string&)>;

// The whole estimate for recordings of dur_a and dur_b seconds (for the progress; the frames decide) at fps. progress: 0 to 1.
// False: failed or cancelled, or the recordings share less than 10 s (the log line says).
bool EstimateOffset(FrameFeed& feed, double dur_a, double dur_b, double fps, SyncResult& result, const SyncLog& log = {},
                    const std::function<void(double)>& progress = {});

// One window of 10 s around A time `center_time`, searched within +-0.6 s of the current offset (seconds). False when
// the recordings do not both cover the window.
bool LocalOffset(FrameFeed& feed, double dur_a, double dur_b, double fps, double center_time, double current_offset,
                 SyncWindow& window);

std::string NowIso();

}  // namespace renderer::clips
