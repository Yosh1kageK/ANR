// Start sync detection and drift tracking in a worker thread: the grey frames come from ffmpeg subprocesses
// (offset_estimate.h does the matching). One job at a time; it can be cancelled, and the viewer polls its progress.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "renderer/clips/offset_estimate.h"

namespace renderer::clips {

class SyncJob {
public:
    ~SyncJob() { Cancel(); }

    struct Recording {
        std::string path;
        double duration = 0.0;  // seconds
    };
    // Detects the offset of the whole recordings and tracks it over them (coarse, then dense windows).
    void StartFull(const Recording& a, const Recording& b, double fps);
    // One window around A time `center` (seconds) at the current offset K(center) (seconds).
    void StartLocal(const Recording& a, const Recording& b, double fps, double center, double current_offset);
    void Cancel();  // stops the job and waits for it
    void Clear();   // Cancel, and forget the result, the window and the error of a finished job (another pair is open)

    bool running() const { return running_; }
    struct Status {
        bool running = false;
        bool local = false;
        double progress = 0.0;
        std::string line;   // the latest log line
        std::string error;  // a failed job
        std::optional<SyncResult> result;  // a finished full job (taken by TakeResult)
        std::optional<SyncWindow> window;  // a finished local job
    };
    Status Poll();
    std::optional<SyncResult> TakeResult();
    std::optional<SyncWindow> TakeWindow();

private:
    void Join();

    std::thread thread_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::mutex mu_;
    Status status_;
    friend class FfmpegFeed;
};

// Grey frames from ffmpeg, each of the recording's own frames once (no resampling, so the numbers are the player's):
// `ffmpeg [-ss S] -i FILE -an -fps_mode passthrough [-frames:v N] -vf scale=W:H:flags=area,format=gray -f rawvideo -`.
class FfmpegFeed : public FrameFeed {
public:
    FfmpegFeed(std::string path_a, std::string path_b, double fps, const std::atomic<bool>* cancel = nullptr)
        : path_{std::move(path_a), std::move(path_b)}, fps_(fps), cancel_(cancel) {}
    bool Frames(int video, int64_t first, int64_t count, int w, int h, const Sink& sink) override;
    bool Cancelled() const override { return cancel_ && cancel_->load(); }

private:
    std::string path_[2];
    double fps_;
    const std::atomic<bool>* cancel_;
};

}  // namespace renderer::clips
