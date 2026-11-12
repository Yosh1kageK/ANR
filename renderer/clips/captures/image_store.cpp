#include "renderer/clips/captures/image_store.h"

#include <algorithm>
#include <chrono>

#include "renderer/clips/captures/recording.h"
#include <spdlog/spdlog.h>

namespace renderer::clips::captures {

void ImageStore::Init(Gfx& gfx, uint64_t limit_bytes) {
    gfx_ = &gfx;
    limit_ = limit_bytes;
    worker_ = std::thread([this] { Run(); });
}

void ImageStore::Destroy() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    if (gfx_ && gfx_->NRI.HasCore() && gfx_->device) {
        gfx_->NRI.DeviceWaitIdle(gfx_->device);
        for (auto& [id, image] : images_)
            Release(*image);
    }
    images_.clear();
    gfx_ = nullptr;
}

void ImageStore::Release(GpuImage& image) {
    gfx_->Release(image.resources);
    bytes_ -= std::min<uint64_t>(bytes_, (uint64_t)image.width * image.height * 8);
}

void ImageStore::Clear() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.clear();
        done_.clear();
    }
    if (gfx_) {
        gfx_->NRI.DeviceWaitIdle(gfx_->device);
        for (auto& [id, image] : images_)
            Release(*image);
    }
    images_.clear();
    failed_.clear();
    requested_.clear();
}

int ImageStore::loading() const {
    std::lock_guard<std::mutex> lock(mu_);
    return (int)(queue_.size() + done_.size());
}

const GpuImage* ImageStore::Get(const Source& source, uint32_t out_w, uint32_t out_h, State& state, std::string& error) {
    const std::string key = ImageKey(source);
    auto it = images_.find(key);
    if (it != images_.end()) {
        it->second->used = ++tick_;
        state = State::READY;
        return it->second.get();
    }
    auto failed = failed_.find(key);
    if (failed != failed_.end()) {
        state = State::FAILED;
        error = failed->second;
        return nullptr;
    }
    state = State::LOADING;
    if (!requested_[key]) {
        requested_[key] = true;
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back({source, out_w, out_h});
        cv_.notify_one();
    }
    return nullptr;
}

void ImageStore::Run() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        const auto start = std::chrono::steady_clock::now();
        Loaded loaded;
        loaded.id = ImageKey(request.source);
        loaded.coding = request.source.coding;
        ReadSource(request.source, request.out_w, request.out_h, loaded.image, loaded.error);
        loaded.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::lock_guard<std::mutex> lock(mu_);
        done_.push_back(std::move(loaded));
    }
}

bool ImageStore::Upload(Loaded& loaded) {
    Image& image = loaded.image;
    auto gpu = std::make_unique<GpuImage>();
    gpu->id = loaded.id;
    gpu->width = image.width;
    gpu->height = image.height;
    gpu->dxgi = image.dxgi;
    gpu->coding = loaded.coding;
    gpu->load_seconds = loaded.seconds;
    auto& NRI = gfx_->NRI;
    gpu->texture = gfx_->NewTexture(gpu->resources, nri::Format::RGBA16_SFLOAT, image.width, image.height,
                                    nri::TextureUsageBits::SHADER_RESOURCE);
    if (!gpu->texture || !gfx_->BindMemory(gpu->resources, 0, 0, nri::MemoryLocation::DEVICE)) {
        gfx_->Release(gpu->resources);
        return false;
    }
    gpu->view = gfx_->TextureView(gpu->resources, gpu->texture, nri::TextureView::TEXTURE, nri::Format::RGBA16_SFLOAT);
    nri::TextureSubresourceUploadDesc sub = {};
    sub.slices = image.rgba.data();
    sub.sliceNum = 1;
    sub.rowPitch = image.width * 8;
    sub.slicePitch = (uint64_t)image.width * 8 * image.height;
    nri::TextureUploadDesc upload = {};
    upload.subresources = &sub;
    upload.texture = gpu->texture;
    upload.after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE};
    if (NRI.UploadData(*gfx_->queue, &upload, 1, nullptr, 0) != nri::Result::SUCCESS) {
        gfx_->Release(gpu->resources);
        return false;
    }
    bytes_ += (uint64_t)image.width * image.height * 8;
    gpu->used = ++tick_;
    images_[loaded.id] = std::move(gpu);
    return true;
}

void ImageStore::Pump(const std::vector<std::string>& keep) {
    Loaded loaded;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!done_.empty()) {
            loaded = std::move(done_.front());
            done_.pop_front();
            have = true;
        }
    }
    if (have) {
        if (!loaded.error.empty()) {
            failed_[loaded.id] = loaded.error;
            spdlog::warn("source {}: {}", loaded.id, loaded.error);
        } else if (!Upload(loaded)) {
            failed_[loaded.id] = "cannot upload the image to the GPU";
        }
    }
    // The limit: the least recently used that are not in use
    while (bytes_ > limit_) {
        GpuImage* oldest = nullptr;
        for (auto& [id, image] : images_)
            if (std::find(keep.begin(), keep.end(), id) == keep.end() && (!oldest || image->used < oldest->used))
                oldest = image.get();
        if (!oldest)
            break;
        gfx_->NRI.DeviceWaitIdle(gfx_->device);
        const std::string id = oldest->id;
        Release(*oldest);
        images_.erase(id);
        requested_.erase(id);
    }
}

}  // namespace renderer::clips::captures
