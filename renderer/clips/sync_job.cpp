#include "renderer/clips/sync_job.h"

#include <cstdio>

#include "renderer/clips/process.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {

bool FfmpegFeed::Frames(int video, int64_t first, int64_t count, int w, int h, const Sink& sink) {
    char seek[48], limit[32], filter[96];
    // A seek lands half a frame before the first wanted one: the frame before it is dropped, that one is the first out.
    std::snprintf(seek, sizeof(seek), "%.6f", ((double)first - 0.5) / fps_);
    std::snprintf(limit, sizeof(limit), "%lld", (long long)count);
    std::snprintf(filter, sizeof(filter), "scale=%d:%d:flags=area,format=gray", w, h);
    std::vector<std::string> args = {FindTool("ffmpeg", "NR_FFMPEG"), "-nostdin", "-v", "error"};
    if (first > 0)
        args.insert(args.end(), {"-ss", seek});
    args.insert(args.end(), {"-i", path_[video], "-an", "-fps_mode", "passthrough"});
    if (count >= 0)
        args.insert(args.end(), {"-frames:v", limit});
    args.insert(args.end(), {"-vf", filter, "-f", "rawvideo", "-"});
    Process process;
    ProcessOptions options;
    options.pipe_stdout = true;
    std::string error;
    if (!process.Start(args, options, &error)) {
        spdlog::error("sync detection: {}", error);
        return false;
    }
    const size_t frame_bytes = (size_t)w * h;
    std::vector<uint8_t> pending;  // a partial frame between reads
    std::vector<uint8_t> chunk(1 << 20);
    int64_t frames = 0;
    bool stopped = false;
    for (size_t n; !stopped && (n = process.Read(chunk.data(), chunk.size())) > 0;) {
        size_t at = 0;
        if (!pending.empty()) {
            const size_t take = std::min(frame_bytes - pending.size(), n);
            pending.insert(pending.end(), chunk.begin(), chunk.begin() + take);
            at = take;
            if (pending.size() == frame_bytes) {
                stopped = !sink(first + frames++, pending.data());
                pending.clear();
            }
        }
        for (; !stopped && n - at >= frame_bytes; at += frame_bytes)
            stopped = !sink(first + frames++, chunk.data() + at);
        if (!stopped && at < n)
            pending.insert(pending.end(), chunk.begin() + at, chunk.begin() + n);
        if (Cancelled())
            stopped = true;
    }
    if (stopped) {
        process.Kill();
        process.CloseStdout();
        process.Wait();
        return false;
    }
    process.CloseStdout();
    const int code = process.Wait();
    if (code != 0 || frames == 0) {
        spdlog::error("sync detection: ffmpeg gave no frames of {} (from frame {}, exit {})", path_[video], (long long)first, code);
        return false;
    }
    return true;
}

void SyncJob::Join() {
    if (thread_.joinable())
        thread_.join();
}

void SyncJob::Cancel() {
    cancel_ = true;
    Join();
    running_ = false;
    cancel_ = false;
}

void SyncJob::Clear() {
    Cancel();
    std::lock_guard<std::mutex> lock(mu_);
    status_ = Status();
}

void SyncJob::StartFull(const Recording& a, const Recording& b, double fps) {
    Cancel();
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = Status();
        status_.running = true;
        status_.line = "matching frames";
    }
    running_ = true;
    thread_ = std::thread([this, a, b, fps] {
        FfmpegFeed feed(a.path, b.path, fps, &cancel_);
        SyncResult result;
        const bool ok = EstimateOffset(
            feed, a.duration, b.duration, fps, result,
            [this](const std::string& line) {
                spdlog::info("sync: {}", line);
                std::lock_guard<std::mutex> lock(mu_);
                status_.line = line;
            },
            [this](double p) {
                std::lock_guard<std::mutex> lock(mu_);
                status_.progress = p;
            });
        std::lock_guard<std::mutex> lock(mu_);
        status_.running = false;
        if (ok)
            status_.result = result;
        else
            status_.error = cancel_ ? "cancelled" : "no result: " + status_.line + " (see the log)";  // the latest log line says why
        running_ = false;
    });
}

void SyncJob::StartLocal(const Recording& a, const Recording& b, double fps, double center, double current_offset) {
    Cancel();
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = Status();
        status_.running = true;
        status_.local = true;
        status_.line = "matching frames around the playhead";
    }
    running_ = true;
    thread_ = std::thread([this, a, b, fps, center, current_offset] {
        FfmpegFeed feed(a.path, b.path, fps, &cancel_);
        SyncWindow window;
        const bool ok = LocalOffset(feed, a.duration, b.duration, fps, center, current_offset, window);
        std::lock_guard<std::mutex> lock(mu_);
        status_.running = false;
        if (ok)
            status_.window = window;
        else
            status_.error = cancel_ ? "cancelled" : "the recordings do not both cover 10 s around the playhead";
        running_ = false;
    });
}

SyncJob::Status SyncJob::Poll() {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

std::optional<SyncResult> SyncJob::TakeResult() {
    std::lock_guard<std::mutex> lock(mu_);
    auto r = status_.result;
    status_.result.reset();
    return r;
}

std::optional<SyncWindow> SyncJob::TakeWindow() {
    std::lock_guard<std::mutex> lock(mu_);
    auto r = status_.window;
    status_.window.reset();
    return r;
}

}  // namespace renderer::clips
