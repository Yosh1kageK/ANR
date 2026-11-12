// The images on the GPU: a source is read (and, for depth and motion, coloured) on a worker thread and uploaded as an
// RGBA16F texture, one per frame of the UI at most. The store keeps the images in use (A, B) and the most recent others
// within a memory limit, and frees the rest. Nothing here writes a file.

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

#include "renderer/clips/captures/capture_reader.h"
#include "renderer/clips/captures/sources.h"
#include "renderer/clips/gfx.h"

namespace renderer::clips::captures {

struct GpuImage {
    std::string id;
    uint32_t width = 0, height = 0;
    int dxgi = 0;
    Coding coding = Coding::SRGB8;
    nri::Texture* texture = nullptr;
    nri::Descriptor* view = nullptr;
    GfxResources resources;
    uint64_t used = 0;       // the last Get
    double load_seconds = 0.0;
};

class ImageStore {
public:
    ~ImageStore() { Destroy(); }
    void Init(Gfx& gfx, uint64_t limit_bytes = 1536ull << 20);
    void Destroy();

    enum class State { LOADING, READY, FAILED };
    // The image of a source (asks for it when it is not here). null while it loads; `state` and `error` say how it stands.
    // `out_w`, `out_h`: the size false colour is made at (the capture's output size).
    const GpuImage* Get(const Source& source, uint32_t out_w, uint32_t out_h, State& state, std::string& error);
    // Uploads what the worker has read (one image per call) and frees what the limit cannot keep. `keep`: the keys (ImageKey) in use.
    void Pump(const std::vector<std::string>& keep);
    // Forgets everything (another capture opened); waits for the GPU.
    void Clear();
    int loading() const;
    uint64_t bytes() const { return bytes_; }

private:
    struct Loaded {
        std::string id;
        Image image;
        Coding coding = Coding::SRGB8;
        std::string error;
        double seconds = 0.0;
    };
    struct Request {
        Source source;
        uint32_t out_w = 0, out_h = 0;
    };
    void Run();
    bool Upload(Loaded& loaded);
    void Release(GpuImage& image);

    Gfx* gfx_ = nullptr;
    uint64_t limit_ = 0, bytes_ = 0, tick_ = 0;
    std::map<std::string, std::unique_ptr<GpuImage>> images_;
    std::map<std::string, std::string> failed_;
    std::map<std::string, bool> requested_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::deque<Loaded> done_;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace renderer::clips::captures
