#include "renderer/clips/offset_estimate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <future>
#include <mutex>
#include <numeric>
#include <thread>

namespace renderer::clips {
namespace {

constexpr int CX = VideoFeatures::CELLS_X, CY = VideoFeatures::CELLS_Y, CELLS = VideoFeatures::CELLS;
constexpr int QX = CX / 2, QY = CY / 2, QCELLS = QX * QY;  // the coarse luma scale: 16x9
constexpr int DIM = CELLS + QCELLS + CELLS;
constexpr float WEIGHT_L32 = 0.3f, WEIGHT_L16 = 0.3f, WEIGHT_G32 = 0.4f;

float Dot(const float* a, const float* b, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += a[i] * b[i];
    return sum;
}

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n == 0 ? 0.0 : (n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]));
}

template <class F>
void ParallelFor(int n, F fn) {
    const int threads = std::max(1, std::min<int>(n, (int)std::thread::hardware_concurrency()));
    if (threads <= 1) {
        for (int i = 0; i < n; i++)
            fn(i);
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&] {
            for (int i; (i = next.fetch_add(1)) < n;)
                fn(i);
        });
    for (std::thread& t : pool)
        t.join();
}

// A block of values, mean-removed over the valid cells and scaled to length sqrt(weight); invalid cells are zero.
void NormalizeBlock(const float* v, const uint8_t* valid, int n, float weight, float* out) {
    double mean = 0.0;
    int count = 0;
    for (int i = 0; i < n; i++)
        if (valid[i]) {
            mean += v[i];
            count++;
        }
    if (count == 0) {
        std::fill(out, out + n, 0.0f);
        return;
    }
    mean /= count;
    double norm = 0.0;
    for (int i = 0; i < n; i++) {
        out[i] = valid[i] ? (float)((double)v[i] - mean) : 0.0f;
        norm += (double)out[i] * out[i];
    }
    const float scale = std::sqrt(weight) / (float)(std::sqrt(norm) + 1e-6);
    for (int i = 0; i < n; i++)
        out[i] *= scale;
}

}  // namespace

std::string NowIso() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const long micros = (long)duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
    std::tm utc = {};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char text[64];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%06ld+00:00", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, micros);
    return text;
}

// --- Features ----------------------------------------------------------------------------------------------------------

void VideoFeatures::Add(const uint8_t* grey) {
    constexpr int W = FEED_W, H = FEED_H, S = W / CELLS_X;  // 4 pixels per cell side
    if (sum.empty()) {
        sum.assign((size_t)W * H, 0.0);
        sum2.assign((size_t)W * H, 0.0);
    }
    cells.resize((size_t)(count + 1) * BYTES);
    uint8_t* out = cells.data() + (size_t)count * BYTES;
    float luma[CELLS] = {}, edge[CELLS] = {};
    for (int y = 0; y < H; y++) {
        const uint8_t* r0 = grey + (size_t)(y > 0 ? y - 1 : y) * W;
        const uint8_t* r1 = grey + (size_t)y * W;
        const uint8_t* r2 = grey + (size_t)(y < H - 1 ? y + 1 : y) * W;
        double* s = sum.data() + (size_t)y * W;
        double* s2 = sum2.data() + (size_t)y * W;
        const int row = (y / S) * CELLS_X;
        for (int x = 0; x < W; x++) {
            const int xl = x > 0 ? x - 1 : x, xr = x < W - 1 ? x + 1 : x;
            const int gx = (r0[xr] + 2 * r1[xr] + r2[xr]) - (r0[xl] + 2 * r1[xl] + r2[xl]);
            const int gy = (r2[xl] + 2 * r2[x] + r2[xr]) - (r0[xl] + 2 * r0[x] + r0[xr]);
            const int c = row + x / S;
            edge[c] += std::sqrt((float)(gx * gx + gy * gy));
            luma[c] += (float)r1[x];
            s[x] += r1[x];
            s2[x] += (double)r1[x] * r1[x];
        }
    }
    for (int c = 0; c < CELLS; c++) {
        out[c] = (uint8_t)std::min(255.0f, luma[c] / (S * S) + 0.5f);
        out[CELLS + c] = (uint8_t)std::min(255.0f, edge[c] / (S * S) * 2.0f + 0.5f);
    }
    count++;
}

std::vector<uint8_t> StaticCells(const VideoFeatures& f) {
    constexpr int W = FEED_W, H = FEED_H, S = W / CX;
    std::vector<uint8_t> flags(CELLS, 0);
    if (f.count < 30)
        return flags;
    std::vector<float> deviation((size_t)W * H);
    for (size_t i = 0; i < deviation.size(); i++) {
        const double mean = f.sum[i] / f.count;
        deviation[i] = (float)std::sqrt(std::max(0.0, f.sum2[i] / f.count - mean * mean));
    }
    std::vector<float> sorted = deviation;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    // static: far below the typical change over time (a quarter of the median), and at least a grey level and a half
    const float limit = std::max(1.5f, 0.25f * sorted[sorted.size() / 2]);
    std::vector<int> quiet(CELLS, 0);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (deviation[(size_t)y * W + x] < limit)
                quiet[(y / S) * CX + x / S]++;
    for (int c = 0; c < CELLS; c++)
        flags[c] = quiet[c] >= S * S * 3 / 4;  // three quarters of the cell's pixels
    return flags;
}

std::vector<uint8_t> CombineMasks(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double* fraction) {
    std::vector<uint8_t> mask(CELLS, 0);
    int count = 0;
    for (int c = 0; c < CELLS; c++) {
        mask[c] = (c < (int)a.size() && a[c]) || (c < (int)b.size() && b[c]);
        count += mask[c];
    }
    if (count > CELLS * 6 / 10) {  // a still recording: nothing would be left to match
        std::fill(mask.begin(), mask.end(), 0);
        count = 0;
    }
    if (fraction)
        *fraction = (double)count / CELLS;
    return mask;
}

Descriptors BuildDescriptors(const VideoFeatures& f, int64_t first, int count, int step, const std::vector<uint8_t>& mask) {
    Descriptors d;
    d.dim = DIM;
    int n = 0;
    while (n < count && first + (int64_t)n * step >= f.base && first + (int64_t)n * step < f.base + f.count)
        n++;
    d.count = n;
    d.data.assign((size_t)n * DIM, 0.0f);
    uint8_t valid32[CELLS], valid16[QCELLS];
    for (int c = 0; c < CELLS; c++)
        valid32[c] = !(c < (int)mask.size() && mask[c]);
    for (int qy = 0; qy < QY; qy++)
        for (int qx = 0; qx < QX; qx++) {
            int ok = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++)
                    ok += valid32[(2 * qy + dy) * CX + 2 * qx + dx];
            valid16[qy * QX + qx] = ok >= 3;
        }
    float l32[CELLS], g32[CELLS], l16[QCELLS];
    for (int i = 0; i < n; i++) {
        const uint8_t* cells = f.frame(first + (int64_t)i * step);
        for (int c = 0; c < CELLS; c++) {
            l32[c] = (float)cells[c];
            g32[c] = std::sqrt((float)cells[CELLS + c]);  // the edge magnitude with its range compressed
        }
        for (int qy = 0; qy < QY; qy++)
            for (int qx = 0; qx < QX; qx++) {
                float sum = 0.0f;
                int ok = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        const int c = (2 * qy + dy) * CX + 2 * qx + dx;
                        if (valid32[c]) {
                            sum += l32[c];
                            ok++;
                        }
                    }
                l16[qy * QX + qx] = ok ? sum / ok : 0.0f;
            }
        float* row = d.data.data() + (size_t)i * DIM;
        NormalizeBlock(l32, valid32, CELLS, WEIGHT_L32, row);
        NormalizeBlock(l16, valid16, QCELLS, WEIGHT_L16, row + CELLS);
        NormalizeBlock(g32, valid32, CELLS, WEIGHT_G32, row + CELLS + QCELLS);
    }
    return d;
}

// --- Coarse ------------------------------------------------------------------------------------------------------------

bool CoarseOffset(const Descriptors& a, const Descriptors& b, double cf, SyncCoarse& out) {
    // The mean similarity of A frame i and B frame i - lag along each diagonal, for every lag with 10 s of overlap.
    const int na = a.count, nb = b.count, dim = a.dim;
    const int min_overlap = (int)std::lround(10.0 * cf);
    std::vector<int> lag_list;
    for (int lag = -(nb - 1); lag < na; lag++) {
        const int n = std::min(na - std::max(lag, 0), nb - std::max(-lag, 0));
        if (n >= min_overlap)
            lag_list.push_back(lag);
    }
    SyncCoarse c;
    if (lag_list.empty())
        return false;
    std::vector<double> mean(lag_list.size());
    ParallelFor((int)lag_list.size(), [&](int index) {
        const int lag = lag_list[(size_t)index];
        const int n = std::min(na - std::max(lag, 0), nb - std::max(-lag, 0));
        const int i0 = std::max(lag, 0), j0 = std::max(-lag, 0);
        double sum = 0.0;
        for (int k = 0; k < n; k++)
            sum += Dot(a.row(i0 + k), b.row(j0 + k), dim);
        mean[(size_t)index] = sum / n;
    });
    std::vector<std::pair<double, double>> lags;  // (offset seconds, mean similarity)
    for (size_t i = 0; i < lag_list.size(); i++)
        lags.push_back({(double)lag_list[i] / cf, mean[i]});
    const auto best = *std::max_element(lags.begin(), lags.end(),
                                        [](const auto& x, const auto& y) { return x.second < y.second; });
    bool have_other = false;
    std::pair<double, double> other = {0.0, -1.0};
    for (const auto& r : lags)
        if (std::fabs(r.first - best.first) > 1.0 && (!have_other || r.second > other.second)) {
            other = r;
            have_other = true;
        }
    double mu = 0.0;
    for (const auto& r : lags)
        mu += r.second;
    mu /= lags.size();
    double variance = 0.0;
    for (const auto& r : lags)
        variance += (r.second - mu) * (r.second - mu);
    const double deviation = std::sqrt(variance / lags.size());
    c.offset = best.first;
    c.similarity = best.second;
    c.next_best = other.second;
    c.next_best_offset = other.first;
    c.z = deviation > 0.0 ? (best.second - mu) / deviation : 0.0;
    out = c;
    return true;
}

// --- One window --------------------------------------------------------------------------------------------------------

SyncWindow TrackWindow(const VideoFeatures& a, const VideoFeatures& b, const std::vector<uint8_t>& mask, int64_t a0, int len,
                       double center, int radius, double fps) {
    SyncWindow w;
    w.a_time = ((double)a0 + 0.5 * len) / fps;
    w.offset = center / fps;
    w.used = false;
    const int64_t kc = (int64_t)std::llround(center), k_lo = kc - radius;
    const int count = 2 * radius + 1;
    const Descriptors da = BuildDescriptors(a, a0, len, 1, mask);
    // B frames the lags reach: a0 + i - k for k in [k_lo, k_hi]
    const int64_t b_first = std::max<int64_t>(a0 - (kc + radius), b.base);
    const int64_t b_last = std::min<int64_t>(a0 + len - k_lo, b.base + b.count);  // exclusive
    const Descriptors db = BuildDescriptors(b, b_first, (int)std::max<int64_t>(b_last - b_first, 0), 1, mask);
    if (da.count < len || db.count == 0)
        return w;
    std::vector<double> curve(count, -2.0);  // -2: no lag (too little overlap)
    ParallelFor(count, [&](int index) {
        const int64_t k = k_lo + index;
        int n = 0;
        double sum = 0.0;
        for (int i = 0; i < len; i++) {
            const int64_t j = a0 + i - k - b_first;
            if (j < 0 || j >= db.count)
                continue;
            sum += Dot(da.row(i), db.row((int)j), da.dim);
            n++;
        }
        if (n >= len / 2)
            curve[(size_t)index] = sum / n;
    });
    int best = -1;
    for (int i = 0; i < count; i++)
        if (curve[(size_t)i] > -2.0 && (best < 0 || curve[(size_t)i] > curve[(size_t)best]))
            best = i;
    if (best < 0)
        return w;
    std::vector<double> valid;
    for (double v : curve)
        if (v > -2.0)
            valid.push_back(v);
    const double peak = curve[(size_t)best];
    w.peak = peak;
    w.contrast = peak - Median(valid);
    // the width of the top: the lags within 0.05 of the peak (a measure of its curvature, whatever the range of lags)
    w.width_frames = (int)std::count_if(valid.begin(), valid.end(), [&](double v) { return v > peak - 0.05; });
    // a peak at the end of the lags (or next to a lag with no data) may be a slope of a higher one outside: not a result
    const bool at_edge = best == 0 || best == count - 1 || curve[(size_t)best - 1] <= -2.0 || curve[(size_t)best + 1] <= -2.0;
    double frac = 0.0;
    if (!at_edge) {
        // the top of the peak as a parabola, by least squares over the lags within a quarter of its width of the best one
        // (a sharp peak: the vertex of the parabola through three lags; a broad flat top is steadier over more)
        int h = std::clamp(w.width_frames / 4, 1, 5);
        for (int k = 1; k <= h; k++)
            if (best - k < 0 || best + k >= count || curve[(size_t)(best - k)] <= -2.0 || curve[(size_t)(best + k)] <= -2.0) {
                h = k - 1;
                break;
            }
        double s0 = 0.0, s2 = 0.0, s4 = 0.0, t0 = 0.0, t1 = 0.0, t2 = 0.0;
        for (int x = -h; x <= h; x++) {
            const double y = curve[(size_t)(best + x)], x2 = (double)x * x;
            s0 += 1.0;
            s2 += x2;
            s4 += x2 * x2;
            t0 += y;
            t1 += x * y;
            t2 += x2 * y;
        }
        const double det = s4 * s0 - s2 * s2;
        if (h >= 1 && det > 0.0) {
            const double a = (t2 * s0 - s2 * t0) / det, b = t1 / s2;
            if (a < 0.0)
                frac = std::clamp(-b / (2.0 * a), -1.0, 1.0);
        }
    }
    w.offset = ((double)k_lo + best + frac) / fps;
    // confidence: the contrast over the median (0.05 to 0.30 is 0 to 1), lower for a broad peak
    w.confidence = std::clamp((w.contrast - 0.05) / 0.25, 0.0, 1.0) * std::min(1.0, 15.0 / std::max(1, w.width_frames));
    w.used = !at_edge && WindowSharp(w);
    return w;
}

// --- The fit -----------------------------------------------------------------------------------------------------------

namespace {

struct Point {
    double t, k;
};

struct Model {
    double slope = 0.0;
    std::vector<double> intercept;
    std::vector<double> residual;  // per point
};

// One slope for all segments (Theil-Sen over the pairs within a segment that are between min_dt and max_dt frames apart,
// when there are enough of them), a median intercept per segment. `splits`: the A frames where each later segment begins.
Model FitSegments(const std::vector<Point>& p, const std::vector<double>& splits, double min_dt, double max_dt = 1e18) {
    Model m;
    std::vector<int> segment(p.size(), 0);
    for (size_t i = 0; i < p.size(); i++)
        for (double s : splits)
            segment[i] += p[i].t >= s;
    std::vector<double> slopes;
    for (int pass = 0; pass < 2 && slopes.size() < 8; pass++) {
        slopes.clear();
        for (size_t i = 0; i < p.size(); i++)
            for (size_t j = i + 1; j < p.size(); j++)
                if (segment[i] == segment[j] && p[j].t - p[i].t >= (pass == 0 ? min_dt : 1.0) &&
                    (pass == 1 || p[j].t - p[i].t <= max_dt))
                    slopes.push_back((p[j].k - p[i].k) / (p[j].t - p[i].t));
    }
    m.slope = slopes.empty() ? 0.0 : Median(slopes);
    m.intercept.assign(splits.size() + 1, 0.0);
    for (size_t s = 0; s < m.intercept.size(); s++) {
        std::vector<double> values;
        for (size_t i = 0; i < p.size(); i++)
            if (segment[i] == (int)s)
                values.push_back(p[i].k - m.slope * p[i].t);
        m.intercept[s] = Median(values);
    }
    m.residual.resize(p.size());
    for (size_t i = 0; i < p.size(); i++)
        m.residual[i] = p[i].k - m.intercept[(size_t)segment[i]] - m.slope * p[i].t;
    return m;
}

constexpr double JUMP_FRAMES = 1.5;  // a sustained residual above this is a jump; a lone one is a rejected window
constexpr double JUMP_SIGMAS = 3.5;  // and a step has to be this many times the windows' own scatter (noisy slow scenes)

}  // namespace

TrackFit FitOffsets(std::vector<SyncWindow>& windows, double fps) {
    TrackFit fit;
    for (SyncWindow& w : windows)
        w.residual_frames = 0.0;
    std::vector<size_t> kept;
    for (size_t i = 0; i < windows.size(); i++)
        if (windows[i].used)
            kept.push_back(i);
    std::sort(kept.begin(), kept.end(), [&](size_t x, size_t y) { return windows[x].a_time < windows[y].a_time; });
    auto points_of = [&](const std::vector<size_t>& idx) {
        std::vector<Point> p;
        for (size_t i : idx)
            p.push_back({windows[i].a_time * fps, windows[i].offset * fps});
        return p;
    };
    auto min_dt_of = [&](const std::vector<Point>& p) { return std::max((p.back().t - p.front().t) / 4.0, 10.0 * fps); };
    // Segments: split where the residuals of the fit step by more than JUMP_FRAMES between two runs of at least two
    // windows, the largest step first, until there is none; the slope is shared. The slope here is from windows 8 to 28 s
    // apart, which a jump spoils only for the pairs across it (a long baseline would be mostly such pairs).
    auto find_jumps = [&](const std::vector<Point>& p) {
        std::vector<double> splits;
        for (int iteration = 0; iteration < 8; iteration++) {
            const Model model = FitSegments(p, splits, 8.0 * fps, 28.0 * fps);
            std::vector<int> segment(p.size(), 0);
            for (size_t i = 0; i < p.size(); i++)
                for (double s : splits)
                    segment[i] += p[i].t >= s;
            // a step has to stand out of the windows' own scatter as well (broad peaks, a slow scene, measure a few frames
            // off): that is read from the differences of neighbours, which a jump spoils only once
            std::vector<double> differences;
            for (size_t i = 0; i + 1 < p.size(); i++)
                differences.push_back(std::fabs((p[i + 1].k - p[i].k) - model.slope * (p[i + 1].t - p[i].t)));
            const double scatter = 1.4826 * Median(differences) / std::sqrt(2.0);
            const double threshold = std::max(JUMP_FRAMES, JUMP_SIGMAS * scatter);
            double best_step = threshold, best_at = 0.0;
            bool found = false;
            for (size_t first = 0; first < p.size();) {
                size_t end = first;
                while (end < p.size() && segment[end] == segment[first])
                    end++;
                for (size_t cut = first + 2; cut + 2 <= end; cut++) {
                    const double left = Median(std::vector<double>(model.residual.begin() + first, model.residual.begin() + cut));
                    const double right = Median(std::vector<double>(model.residual.begin() + cut, model.residual.begin() + end));
                    if (std::fabs(right - left) > best_step) {
                        best_step = std::fabs(right - left);
                        best_at = 0.5 * (p[cut - 1].t + p[cut].t);
                        found = true;
                    }
                }
                first = end;
            }
            if (!found)
                break;
            splits.push_back(best_at);
            std::sort(splits.begin(), splits.end());
        }
        // The refits move the steps: a split whose step (the difference of its two intercepts, the slope being shared) fell
        // under the threshold goes, the smallest first.
        while (!splits.empty()) {
            const Model model = FitSegments(p, splits, 8.0 * fps, 28.0 * fps);
            std::vector<double> differences;
            for (size_t i = 0; i + 1 < p.size(); i++)
                differences.push_back(std::fabs((p[i + 1].k - p[i].k) - model.slope * (p[i + 1].t - p[i].t)));
            const double threshold = std::max(JUMP_FRAMES, JUMP_SIGMAS * 1.4826 * Median(differences) / std::sqrt(2.0));
            size_t smallest = 0;
            for (size_t i = 1; i < splits.size(); i++)
                if (std::fabs(model.intercept[i + 1] - model.intercept[i]) < std::fabs(model.intercept[smallest + 1] - model.intercept[smallest]))
                    smallest = i;
            if (std::fabs(model.intercept[smallest + 1] - model.intercept[smallest]) >= threshold)
                break;
            splits.erase(splits.begin() + (long)smallest);
        }
        return splits;
    };
    std::vector<double> splits;
    Model model;
    for (int pass = 0; pass < 2; pass++) {
        if (kept.size() < 2)
            return fit;
        const std::vector<Point> p = points_of(kept);
        splits = find_jumps(p);
        model = FitSegments(p, splits, min_dt_of(p));
        if (pass == 1)
            break;
        // The windows off the fit are out: lone ones (a run of them would have been a jump). Then the jumps are found
        // again without them.
        std::vector<double> magnitude;
        for (double r : model.residual)
            magnitude.push_back(std::fabs(r));
        const double tolerance = std::max(JUMP_FRAMES, 3.0 * 1.4826 * Median(magnitude));
        std::vector<size_t> next;
        for (size_t i = 0; i < kept.size(); i++) {
            if (std::fabs(model.residual[i]) <= tolerance)
                next.push_back(kept[i]);
            else
                windows[kept[i]].used = false;
        }
        kept = next;
    }
    const std::vector<Point> p = points_of(kept);
    fit.ok = true;
    fit.slope = model.slope;
    fit.intercepts = model.intercept;
    for (double s : splits) {
        TrackFit::Split split;
        for (const Point& q : p)
            if (q.t < s)
                split.left = q.t;
            else if (split.right == 0.0)
                split.right = q.t;
        fit.splits.push_back(split);
    }
    fit.used = (int)kept.size();
    fit.rejected = (int)windows.size() - fit.used;
    // every window's residual against its own segment's line (the rejected ones too)
    for (SyncWindow& w : windows) {
        size_t segment = 0;
        for (double s : splits)
            segment += w.a_time * fps >= s;
        w.residual_frames = w.offset * fps - fit.At(segment, w.a_time * fps);
    }
    return fit;
}

int64_t RefineJump(const VideoFeatures& a, const VideoFeatures& b, const std::vector<uint8_t>& mask, int64_t t_left,
                   int64_t t_right, double left_b, double right_b, double slope) {
    if (t_right <= t_left)
        return t_left;
    const int n = (int)(t_right - t_left + 1);
    const double k_min = std::min(left_b, right_b) + slope * (double)t_left - 2.0;
    const double k_max = std::max(left_b, right_b) + slope * (double)t_right + 2.0;
    const Descriptors da = BuildDescriptors(a, t_left, n, 1, mask);
    const int64_t b_first = std::max<int64_t>(t_left - (int64_t)std::ceil(k_max), b.base);
    const int64_t b_last = std::min<int64_t>(t_right - (int64_t)std::floor(k_min) + 1, b.base + b.count);
    const Descriptors db = BuildDescriptors(b, b_first, (int)std::max<int64_t>(b_last - b_first, 0), 1, mask);
    if (da.count < n || db.count == 0)
        return (t_left + t_right + 1) / 2;
    // the frame's similarity under each line (none where B has no frame: the same for both, so it does not matter)
    std::vector<double> left(n, 0.0), right(n, 0.0);
    for (int i = 0; i < n; i++) {
        const int64_t frame = t_left + i;
        const int64_t jl = frame - std::llround(left_b + slope * (double)frame) - b_first;
        const int64_t jr = frame - std::llround(right_b + slope * (double)frame) - b_first;
        if (jl >= 0 && jl < db.count)
            left[(size_t)i] = Dot(da.row(i), db.row((int)jl), da.dim);
        if (jr >= 0 && jr < db.count)
            right[(size_t)i] = Dot(da.row(i), db.row((int)jr), da.dim);
    }
    // the split J in [t_left, t_right + 1]: frames before it under the left line, from it on under the right one
    std::vector<double> score(n + 1, 0.0);
    double total_right = 0.0;
    for (double v : right)
        total_right += v;
    double prefix_left = 0.0, prefix_right = 0.0;
    for (int i = 0; i <= n; i++) {
        score[(size_t)i] = prefix_left + (total_right - prefix_right);
        if (i < n) {
            prefix_left += left[(size_t)i];
            prefix_right += right[(size_t)i];
        }
    }
    const double best = *std::max_element(score.begin(), score.end());
    // on a plateau (a pause: no frame matches either line) the middle of it
    std::vector<int> at_best;
    for (int i = 0; i <= n; i++)
        if (score[(size_t)i] >= best - 0.5)
            at_best.push_back(i);
    return t_left + at_best[at_best.size() / 2];
}

// --- The whole estimate ------------------------------------------------------------------------------------------------

bool EstimateOffset(FrameFeed& feed, double dur_a, double dur_b, double fps, SyncResult& result, const SyncLog& log,
                    const std::function<void(double)>& progress) {
    const auto started = std::chrono::steady_clock::now();
    auto say = [&](const std::string& text) {
        if (log)
            log(text);
    };
    std::mutex progress_mutex;
    double reported = 0.0;
    auto report = [&](double f) {
        std::lock_guard<std::mutex> lock(progress_mutex);
        reported = std::max(reported, f);
        if (progress)
            progress(reported);
    };
    report(0.0);

    // 1. Both recordings, once, in their own frames
    VideoFeatures fa, fb;
    {
        const double expected = std::max(1.0, (dur_a + dur_b) * fps);
        std::atomic<int64_t> done{0};
        auto decode = [&](int video, VideoFeatures& f) {
            return feed.Frames(video, 0, -1, FEED_W, FEED_H, [&](int64_t, const uint8_t* grey) {
                f.Add(grey);
                const int64_t n = ++done;
                if (n % 256 == 0)
                    report(0.8 * std::min(1.0, (double)n / expected));
                return !feed.Cancelled();
            });
        };
        auto later = std::async(std::launch::async, [&] { return decode(1, fb); });
        const bool ok_a = decode(0, fa);
        const bool ok_b = later.get();
        if (!ok_a || !ok_b || feed.Cancelled() || fa.count == 0 || fb.count == 0)
            return false;
    }
    report(0.8);
    {
        char text[160];
        std::snprintf(text, sizeof(text), "decoded %lld and %lld frames in %.1f s", (long long)fa.count, (long long)fb.count,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        say(text);
    }

    // 2. What to leave out of the match
    double mask_fraction = 0.0;
    const std::vector<uint8_t> mask = CombineMasks(StaticCells(fa), StaticCells(fb), &mask_fraction);

    // 3. Coarse: about 10 fps over every lag
    const int step = std::max(1, (int)std::lround(fps / 10.0));
    const double cf = fps / step;
    const Descriptors ca = BuildDescriptors(fa, 0, (int)((fa.count + step - 1) / step), step, mask);
    const Descriptors cb = BuildDescriptors(fb, 0, (int)((fb.count + step - 1) / step), step, mask);
    SyncCoarse coarse;
    if (!CoarseOffset(ca, cb, cf, coarse)) {
        say("the recordings share less than 10 s");
        return false;
    }
    {
        char text[160];
        std::snprintf(text, sizeof(text), "coarse: A = B %+.1f s, similarity %.3f (next best %.3f, z %.1f), %.0f%% of the picture masked",
                      coarse.offset, coarse.similarity, coarse.next_best, coarse.z, mask_fraction * 100.0);
        say(text);
    }
    report(0.85);
    if (feed.Cancelled())
        return false;

    SyncResult r;
    r.coarse = coarse;
    r.tracking.mask_fraction = mask_fraction;
    r.detected_at = NowIso();
    r.method = "frame matching: luma at two scales and edges of 128x72 frames, static regions masked; 10 fps over all lags, "
               "then 10 s windows every 4 s at the frame rate, a robust fit of the offset";

    // 4. Dense tracking
    const double k0 = coarse.offset * fps;
    const int64_t k0_round = (int64_t)std::llround(k0);
    const int len = (int)std::lround(10.0 * fps), radius = (int)std::lround(0.6 * fps);
    const int64_t lo = std::max<int64_t>(0, k0_round), hi = std::min<int64_t>(fa.count, fb.count + k0_round);
    // windows whose B frames (within the radius of the coarse offset) all exist
    const int64_t first_start = std::max<int64_t>(lo, k0_round + radius);
    const int64_t last_end = std::min<int64_t>(hi, fb.count + k0_round - radius);
    const int64_t span = last_end - first_start;
    std::vector<SyncWindow> windows;
    std::vector<int64_t> starts;
    if (span >= (int64_t)std::lround(11.5 * fps)) {
        const int64_t spacing = (int64_t)std::lround(4.0 * fps);
        const int n = std::max<int>(1, 1 + (int)std::ceil((double)(span - len) / (double)spacing));
        for (int j = 0; j < n; j++)
            starts.push_back(n == 1 ? first_start + (span - len) / 2 : first_start + (int64_t)std::llround((double)j * (span - len) / (n - 1)));
    }
    if (!starts.empty()) {
        const int n = (int)starts.size();
        // the seed: the sharpest of a few windows spread over the overlap, searched around the coarse offset
        const int seeds = std::min(5, n);
        std::vector<int> seed_index;
        for (int i = 0; i < seeds; i++)
            seed_index.push_back(seeds == 1 ? n / 2 : (int)std::lround((double)i * (n - 1) / (seeds - 1)));
        std::vector<SyncWindow> seed_windows(seeds);
        ParallelFor(seeds, [&](int i) {
            seed_windows[(size_t)i] = TrackWindow(fa, fb, mask, starts[(size_t)seed_index[(size_t)i]], len, k0,
                                                  radius + (int)std::lround(0.1 * fps), fps);
        });
        int best = -1;
        for (int i = 0; i < seeds; i++)
            if (seed_windows[(size_t)i].used && (best < 0 || seed_windows[(size_t)i].confidence > seed_windows[(size_t)best].confidence))
                best = i;
        if (best >= 0) {
            const int seed = seed_index[(size_t)best];
            std::vector<SyncWindow> tracked((size_t)n);
            std::vector<uint8_t> have((size_t)n, 0);
            tracked[(size_t)seed] = seed_windows[(size_t)best];
            have[(size_t)seed] = 1;
            std::atomic<int> finished{1};
            // outward from the seed in both directions, each window searched around the running estimate
            auto chain = [&](int direction) {
                double running = seed_windows[(size_t)best].offset * fps;
                int rejects = 0;
                for (int j = seed + direction; j >= 0 && j < n && !feed.Cancelled(); j += direction) {
                    const int search = std::min(radius + (int)std::lround(0.3 * fps) * rejects, (int)std::lround(1.8 * fps));
                    tracked[(size_t)j] = TrackWindow(fa, fb, mask, starts[(size_t)j], len, running, search, fps);
                    have[(size_t)j] = 1;
                    if (tracked[(size_t)j].used) {
                        running = tracked[(size_t)j].offset * fps;
                        rejects = 0;
                    } else {
                        rejects++;
                    }
                    report(0.85 + 0.13 * (double)(++finished) / n);
                }
            };
            auto other = std::async(std::launch::async, chain, -1);
            chain(+1);
            other.get();
            if (feed.Cancelled())
                return false;
            for (int j = 0; j < n; j++)
                if (have[(size_t)j])
                    windows.push_back(tracked[(size_t)j]);
        }
    }

    // 5. The robust fit, the jumps placed to the frame, the offset map
    const TrackFit fit = FitOffsets(windows, fps);
    r.windows = windows;
    int sharp_count = 0;
    double conf_sum = 0.0;
    for (const SyncWindow& w : windows)
        if (w.used) {
            sharp_count++;
            conf_sum += w.confidence;
        }
    r.tracking.present = true;
    r.tracking.windows_used = sharp_count;
    r.tracking.windows_rejected = (int)windows.size() - sharp_count;
    if (fit.ok) {
        std::vector<double> offsets;
        for (const SyncWindow& w : windows)
            if (w.used)
                offsets.push_back(w.offset);
        r.spread_seconds = *std::max_element(offsets.begin(), offsets.end()) - *std::min_element(offsets.begin(), offsets.end());
        r.tracking.slope_ppm = fit.slope * 1e6;
        r.tracking.drift_frames = fit.slope * (double)(hi - lo);
        r.tracking.confidence = conf_sum / std::max<size_t>(1, windows.size());
        // each jump: where, and by how much
        std::vector<int64_t> jump_at;
        for (size_t s = 0; s < fit.splits.size(); s++) {
            const int64_t j = RefineJump(fa, fb, mask, (int64_t)std::llround(fit.splits[s].left),
                                         (int64_t)std::llround(fit.splits[s].right), fit.intercepts[s], fit.intercepts[s + 1], fit.slope);
            jump_at.push_back(j);
            r.tracking.jumps.push_back({(double)j / fps, fit.intercepts[s + 1] - fit.intercepts[s]});
        }
        r.tracking.significant = std::fabs(r.tracking.drift_frames) > 1.0 || !jump_at.empty();
        auto rounded = [](double k) { return std::round(k * 1000.0) / 1000.0; };
        std::vector<OffsetPoint>& map = r.tracking.map;
        map.push_back({lo, rounded(fit.At(0, (double)lo))});
        for (size_t s = 0; s < jump_at.size(); s++) {
            map.push_back({jump_at[s] - 1, rounded(fit.At(s, (double)(jump_at[s] - 1)))});
            map.push_back({jump_at[s], rounded(fit.At(s + 1, (double)jump_at[s]))});
        }
        map.push_back({std::max(lo, hi - 1), rounded(fit.At(fit.intercepts.size() - 1, (double)std::max(lo, hi - 1)))});
        double k_exact;
        if (r.tracking.significant) {
            k_exact = map.front().k;
            r.quality = jump_at.empty() ? "drifting" : "jumps";
        } else {
            k_exact = fit.At(0, 0.5 * (double)(lo + hi));  // no jump, no real slope: the constant at the middle
            r.quality = sharp_count >= 3 ? "fine" : "weak";
            map.clear();
        }
        r.offset = k_exact / fps;
        r.offset_frames = RoundK(k_exact);
    } else if (sharp_count == 1) {
        // one sharp window: its offset, no model
        for (const SyncWindow& w : windows)
            if (w.used) {
                r.offset = w.offset;
                r.offset_frames = (int64_t)std::nearbyint(w.offset * fps);
                r.quality = "weak";
                r.tracking.confidence = w.confidence;
                r.spread_seconds = 0.0;
            }
    } else {
        // no window: the coarse offset, to a tenth of a second
        r.offset = std::round(coarse.offset * 10.0) / 10.0;
        r.offset_frames = (int64_t)std::nearbyint(r.offset * fps);
        r.quality = "coarse";
        r.tracking.confidence = 0.0;
    }
    {
        char text[256];
        std::snprintf(text, sizeof(text),
                      "tracking: %d of %zu windows used, K %+lld frames, drift %+.2f frames over the overlap (%+.0f ppm), %zu jumps, "
                      "confidence %.2f (%s), %.1f s",
                      r.tracking.windows_used, windows.size(), (long long)r.offset_frames, r.tracking.drift_frames,
                      r.tracking.slope_ppm, r.tracking.jumps.size(), r.tracking.confidence, r.quality.c_str(),
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        say(text);
    }
    report(1.0);
    result = r;
    return true;
}

bool LocalOffset(FrameFeed& feed, double dur_a, double dur_b, double fps, double center_time, double current_offset,
                 SyncWindow& window) {
    const int len = (int)std::lround(10.0 * fps), radius = (int)std::lround(0.6 * fps);
    const double k = current_offset * fps;
    const int64_t k_round = (int64_t)std::llround(k);
    const int64_t frames_a = (int64_t)std::floor(dur_a * fps), frames_b = (int64_t)std::floor(dur_b * fps);
    // inside the overlap, with B's frames of every lag in the recording
    const int64_t lo = std::max<int64_t>(0, k_round + radius), hi = std::min<int64_t>(frames_a, frames_b + k_round - radius);
    if (hi - lo < (int64_t)std::lround(11.5 * fps))
        return false;
    const int64_t a0 = std::min(std::max<int64_t>((int64_t)std::llround(center_time * fps) - len / 2, lo), hi - len);
    const int64_t b0 = std::max<int64_t>(a0 - k_round - radius, 0);
    VideoFeatures fa, fb;
    fa.base = a0;
    fb.base = b0;
    auto later = std::async(std::launch::async, [&] {
        return feed.Frames(1, b0, len + 2 * radius + 2, FEED_W, FEED_H, [&](int64_t, const uint8_t* g) { fb.Add(g); return !feed.Cancelled(); });
    });
    const bool ok_a = feed.Frames(0, a0, len, FEED_W, FEED_H, [&](int64_t, const uint8_t* g) { fa.Add(g); return !feed.Cancelled(); });
    if (!later.get() || !ok_a || feed.Cancelled() || fa.count < len)
        return false;
    const std::vector<uint8_t> mask = CombineMasks(StaticCells(fa), StaticCells(fb));
    window = TrackWindow(fa, fb, mask, a0, len, k, radius, fps);
    return true;
}

}  // namespace renderer::clips
