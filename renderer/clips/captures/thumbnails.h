// Thumbnails of the captures in the browser: the mid-burst HUD-less frame (else the backbuffer), through the SDR curve, 8-bit,
// 384 pixels wide, made on a worker thread (the texture is 66 MB at 4K: read once, sampled, never kept) and cached as
// PNG in the renderer's cache folder under the capture's name and the size of its description file (record.json). The
// browser asks for the
// thumbnails of the cards it shows, at most two at a time.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "renderer/clips/captures/capture_info.h"
#include "renderer/clips/captures/store.h"

namespace renderer::clips::captures {

struct Thumbnail {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// The cache file of a capture's thumbnail.
std::string ThumbnailPath(const Locations& where, const CaptureInfo& capture);
// Reads the capture's frame and samples it down to `width` pixels wide (no cache). Seconds taken in `seconds`.
bool MakeThumbnail(const CaptureInfo& capture, int width, Thumbnail& out, std::string& error);

class ThumbnailLoader {
public:
    explicit ThumbnailLoader(Locations where);
    ~ThumbnailLoader();

    enum class State { NONE, LOADING, READY, FAILED };
    // The state of a capture's thumbnail; asks for it when it is NONE (from a card that is shown). `image` is the RGBA
    // pixels when READY.
    State Request(const CaptureInfo& capture, const Thumbnail** image);
    // How many thumbnails were made (not read from the cache) and the time they took, for the report.
    int made() const { return made_; }
    double made_seconds() const { return made_seconds_; }
    int loading() const;

private:
    void Run();
    struct Item {
        State state = State::NONE;
        Thumbnail image;
        std::string error;
    };
    Locations where_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Item>> items_;
    std::deque<std::pair<CaptureInfo, std::shared_ptr<Item>>> queue_;
    std::atomic<int> made_{0};
    std::atomic<double> made_seconds_{0.0};
    bool stop_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace renderer::clips::captures
