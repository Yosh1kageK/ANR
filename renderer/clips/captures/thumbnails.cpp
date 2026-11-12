#include "renderer/clips/captures/thumbnails.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

#include <Detex/stb_image.h>
#include "renderer/clips/captures/capture_reader.h"
#include "renderer/clips/captures/curves.h"
#include "renderer/clips/captures/png_write.h"
#include "renderer/clips/captures/recording.h"
#include <spdlog/spdlog.h>

namespace renderer::clips::captures {
namespace fs = std::filesystem;

std::string ThumbnailPath(const Locations& where, const CaptureInfo& c) {
    std::error_code ec;
    const FolderFormat* format = FormatOf(c);
    const uintmax_t size = fs::file_size(c.folder + "/" + (format ? format->marker : std::string("record.json")), ec);
    return where.Thumbs() + "/" + c.name + "-" + Hash64(c.folder + ":" + std::to_string(size)).substr(0, 8) + ".png";
}

bool MakeThumbnail(const CaptureInfo& c, int width, Thumbnail& out, std::string& error) {
    const int frame = (int)c.frames.size() / 2;
    const FrameInfo* f = c.Frame(frame);
    if (!f) {
        error = "no frames";
        return false;
    }
    const char* name = f->files.count("hudless") ? "hudless" : "backbuffer";
    auto it = f->files.find(name);
    if (it == f->files.end()) {
        error = "no colour texture";
        return false;
    }
    Image image;
    if (!ReadFrameColor(c, frame, name, image, error))
        return false;
    out.width = width;
    out.height = std::max(1, (int)std::lround((double)width * image.height / image.width));
    out.rgba.assign((size_t)out.width * out.height * 4, 255);
    const int samples = 3;  // per axis inside each thumbnail pixel
    for (int ty = 0; ty < out.height; ty++)
        for (int tx = 0; tx < out.width; tx++) {
            float sum[3] = {0, 0, 0};
            for (int sy = 0; sy < samples; sy++)
                for (int sx = 0; sx < samples; sx++) {
                    const uint64_t x = std::min<uint64_t>((uint64_t)((tx + (sx + 0.5) / samples) * image.width / out.width), image.width - 1);
                    const uint64_t y = std::min<uint64_t>((uint64_t)((ty + (sy + 0.5) / samples) * image.height / out.height), image.height - 1);
                    const uint16_t* p = &image.rgba[((size_t)y * image.width + x) * 4];
                    const float r = HalfToFloat(p[0]), g = HalfToFloat(p[1]), b = HalfToFloat(p[2]);
                    Rgb c3;
                    if (c.coding == Coding::PQ10)
                        c3 = SdrFromPq(r, g, b);
                    else if (c.coding == Coding::SCRGB16)
                        c3 = SdrFromScrgb(r, g, b);
                    else
                        c3 = {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)};
                    sum[0] += c3.r;
                    sum[1] += c3.g;
                    sum[2] += c3.b;
                }
            uint8_t* o = &out.rgba[((size_t)ty * out.width + tx) * 4];
            for (int k = 0; k < 3; k++)
                o[k] = (uint8_t)std::lround(std::clamp(sum[k] / (float)(samples * samples), 0.0f, 1.0f) * 255.0f);
        }
    return true;
}

ThumbnailLoader::ThumbnailLoader(Locations where) : where_(std::move(where)) {
    for (int i = 0; i < 2; i++)
        workers_.emplace_back([this] { Run(); });
}

ThumbnailLoader::~ThumbnailLoader() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_)
        t.join();
}

int ThumbnailLoader::loading() const {
    std::lock_guard<std::mutex> lock(mu_);
    int n = 0;
    for (const auto& [name, item] : items_)
        n += item->state == State::LOADING;
    return n;
}

ThumbnailLoader::State ThumbnailLoader::Request(const CaptureInfo& capture, const Thumbnail** image) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = items_.find(capture.folder);
    if (it == items_.end()) {
        auto item = std::make_shared<Item>();
        item->state = State::LOADING;
        it = items_.emplace(capture.folder, item).first;
        queue_.push_back({capture, item});
        cv_.notify_one();
    }
    if (it->second->state == State::READY && image)
        *image = &it->second->image;
    return it->second->state;
}

void ThumbnailLoader::Run() {
    for (;;) {
        std::pair<CaptureInfo, std::shared_ptr<Item>> work;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            work = std::move(queue_.front());
            queue_.pop_front();
        }
        const std::string path = ThumbnailPath(where_, work.first);
        Thumbnail image;
        std::string error;
        bool ok = false;
        int w = 0, h = 0, channels = 0;
        if (unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4)) {  // the cache
            image.width = w;
            image.height = h;
            image.rgba.assign(pixels, pixels + (size_t)w * h * 4);
            stbi_image_free(pixels);
            ok = true;
        } else {
            const auto start = std::chrono::steady_clock::now();
            ok = MakeThumbnail(work.first, 384, image, error);
            if (ok) {
                std::vector<uint8_t> rgb((size_t)image.width * image.height * 3);
                for (size_t i = 0; i < (size_t)image.width * image.height; i++)
                    for (int k = 0; k < 3; k++)
                        rgb[i * 3 + (size_t)k] = image.rgba[i * 4 + (size_t)k];
                std::string write_error;
                WritePng8(path, (uint32_t)image.width, (uint32_t)image.height, 3, rgb.data(), image.width * 3, write_error);
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                made_++;
                made_seconds_ = made_seconds_ + seconds;
                spdlog::info("thumbnail of {}: {:.2f} s", work.first.name, seconds);
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        work.second->image = std::move(image);
        work.second->error = error;
        work.second->state = ok ? State::READY : State::FAILED;
    }
}

}  // namespace renderer::clips::captures
