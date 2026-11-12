#include "renderer/clips/ffmpeg_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "renderer/clips/frame_cache.h"
#include "renderer/clips/probe.h"
#include "renderer/clips/process.h"
#include "renderer/clips/yuv.h"

namespace renderer::clips {
namespace {

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t Align(uint64_t v, uint64_t a) {
    return a > 1 ? (v + a - 1) / a * a : v;
}

// ffmpeg's pixel format for a bit depth: 8-bit NV12, deeper P010 or P012 (the sample in the high bits of 16).
const char* PixelFormat(uint32_t bits) {
    return bits <= 8 ? "nv12" : bits <= 10 ? "p010le" : bits <= 12 ? "p012le" : "p016le";
}

class FfmpegSource final : public VideoSource {
public:
    ~FfmpegSource() override { Shutdown(); }

    bool Init(const VideoContext& context, const std::string& path, const FfmpegOptions& options, std::string& error);

    const VideoInfo& Info() const override { return info_; }
    void SetInterest(int64_t first, int64_t last, int64_t playhead, int direction) override;
    bool Ready(int64_t frame) const override {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.IsReady(frame);
    }
    bool Acquire(int64_t frame, FrameRef& out) override {
        std::lock_guard<std::mutex> lock(mu_);
        int slot = -1;
        if (!cache_.Pin(frame, slot))
            return false;
        out.index = frame;
        out.luma = slots_[(size_t)slot].luma_view;
        out.chroma = slots_[(size_t)slot].chroma_view;
        out.slot = (uint32_t)slot;
        return true;
    }
    void Release(const FrameRef& frame) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            cache_.Unpin((int)frame.slot);
            version_++;
        }
        cv_.notify_one();
    }
    DecodeStats Stats() const override;
    std::string Describe() const override;

private:
    struct GpuSlot {
        nri::Texture *luma = nullptr, *chroma = nullptr;
        nri::Descriptor *luma_view = nullptr, *chroma_view = nullptr;
    };
    struct Staging {
        nri::Buffer* buffer = nullptr;
        nri::Memory* memory = nullptr;
        uint8_t* data = nullptr;
        nri::CommandAllocator* allocator = nullptr;
        nri::CommandBuffer* command_buffer = nullptr;
        uint64_t fence_value = 0;  // the upload in flight from it (0: none)
    };
    struct InFlight {
        int slot;
        uint64_t value;
    };

    bool CreateGpu(std::string& error);
    void Shutdown();
    void Run();
    bool StartStream(int64_t first_frame);
    bool ReadFrame(Staging& staging);
    void SubmitUpload(int slot, Staging& staging);
    Staging& AcquireStaging();
    void ReapLocked();
    void SettleUploads();
    void RecordFrameTime(double seconds);

    VideoContext context_;
    const nri::CoreInterface& core() const { return *context_.core; }
    VideoInfo info_;
    FfmpegOptions options_;
    std::string path_, ffmpeg_, pixel_format_;
    PlaneLayout planes_;
    uint64_t chroma_offset_ = 0;                   // in a staging buffer
    uint64_t luma_pitch_ = 0, chroma_pitch_ = 0;   // row pitches in a staging buffer
    int64_t skip_limit_ = 90;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    SlotCache cache_;
    FrameWindow window_;
    uint64_t version_ = 0;
    double jumped_at_ = 0.0;  // when the playhead last moved by more than a frame (a seek)
    std::atomic<bool> quit_{false};
    std::atomic<bool> finished_{false};  // the worker thread left Run
    std::string error_;
    int64_t decoded_total_ = 0;
    std::vector<double> frame_times_;
    size_t frame_time_next_ = 0;

    std::vector<GpuSlot> slots_;
    std::vector<nri::Memory*> memory_;
    std::vector<Staging> staging_;
    size_t staging_next_ = 0;
    nri::Fence* fence_ = nullptr;
    uint64_t submitted_ = 0;
    std::deque<InFlight> inflight_;
    std::vector<uint8_t> scratch_;  // when a plane's rows are padded in the staging buffer

    Process process_;
    int64_t stream_next_ = -1;  // the frame the stream delivers next (-1: no stream)
    std::thread thread_;
};

bool FfmpegSource::Init(const VideoContext& context, const std::string& path, const FfmpegOptions& options,
                        std::string& error) {
    context_ = context;
    options_ = options;
    path_ = path;
    ffmpeg_ = options.ffmpeg.empty() ? FindTool("ffmpeg", "NR_FFMPEG") : options.ffmpeg;
    if (!context.device || !context.core || !context.helper || !context.graphics_queue) {
        error = "no graphics device";
        return false;
    }
    ProbeResult probe;
    if (!ProbeVideo(path, probe, error))
        return false;
    info_ = probe.info;
    info_.bit_depth = info_.bit_depth <= 8 ? 8 : info_.bit_depth <= 10 ? 10 : info_.bit_depth <= 12 ? 12 : 16;
    if (probe.variable_frame_rate)
        spdlog::warn("{}: variable frame rate; frame numbers are decoded order, not time / fps", path);
    pixel_format_ = PixelFormat(info_.bit_depth);
    planes_ = MakePlaneLayout(info_.width, info_.height, info_.bit_depth);
    skip_limit_ = 30 + (info_.keyframe_gap > 0 ? info_.keyframe_gap / 2 : 60);
    if (!CreateGpu(error))
        return false;
    frame_times_.assign(24, 0.0);
    {
        std::lock_guard<std::mutex> lock(mu_);
        window_ = {0, std::min<int64_t>(info_.frames - 1, 8), 0};
        version_++;
    }
    thread_ = std::thread([this] { Run(); });
    return true;
}

bool FfmpegSource::CreateGpu(std::string& error) {
    const nri::CoreInterface& c = core();
    const nri::HelperInterface& h = *context_.helper;
    const nri::DeviceDesc& device_desc = c.GetDeviceDesc(*context_.device);
    const uint64_t row_alignment = std::max<uint32_t>(device_desc.memoryAlignment.uploadBufferTextureRow, 1u);
    const uint64_t slice_alignment = std::max<uint32_t>(device_desc.memoryAlignment.uploadBufferTextureSlice, 1u);
    luma_pitch_ = Align(planes_.luma_row_bytes, row_alignment);
    chroma_pitch_ = Align(planes_.chroma_row_bytes, row_alignment);
    chroma_offset_ = Align(luma_pitch_ * planes_.luma_height, slice_alignment);
    const uint64_t staging_bytes = chroma_offset_ + chroma_pitch_ * planes_.chroma_height;
    if (luma_pitch_ != planes_.luma_row_bytes || chroma_pitch_ != planes_.chroma_row_bytes)
        scratch_.resize(planes_.luma_bytes);

    const bool deep = planes_.bytes_per_sample == 2;
    const nri::Format luma_format = deep ? nri::Format::R16_UNORM : nri::Format::R8_UNORM;
    const nri::Format chroma_format = deep ? nri::Format::RG16_UNORM : nri::Format::RG8_UNORM;

    int64_t capacity = (int64_t)(options_.memory_bytes / std::max<uint64_t>(planes_.frame_bytes(), 1));
    capacity = std::clamp<int64_t>(capacity, 12, std::max<int64_t>(info_.frames, 12));
    capacity = std::min<int64_t>(capacity, info_.frames + 4);

    std::vector<nri::Texture*> textures;
    slots_.clear();
    for (int64_t i = 0; i < capacity; i++) {
        nri::TextureDesc desc = {};
        desc.type = nri::TextureType::TEXTURE_2D;
        desc.usage = nri::TextureUsageBits::SHADER_RESOURCE;
        desc.mipNum = 1;
        GpuSlot slot;
        desc.format = luma_format;
        desc.width = (nri::Dim_t)planes_.luma_width;
        desc.height = (nri::Dim_t)planes_.luma_height;
        if (c.CreateTexture(*context_.device, desc, slot.luma) != nri::Result::SUCCESS) {
            error = "out of memory creating the frame cache (video memory)";
            break;
        }
        desc.format = chroma_format;
        desc.width = (nri::Dim_t)planes_.chroma_width;
        desc.height = (nri::Dim_t)planes_.chroma_height;
        if (c.CreateTexture(*context_.device, desc, slot.chroma) != nri::Result::SUCCESS) {
            c.DestroyTexture(slot.luma);
            error = "out of memory creating the frame cache (video memory)";
            break;
        }
        textures.push_back(slot.luma);
        textures.push_back(slot.chroma);
        slots_.push_back(slot);
    }
    if (slots_.size() < 12) {
        for (nri::Texture* t : textures)
            c.DestroyTexture(t);
        slots_.clear();
        if (error.empty())
            error = "cannot create the frame cache";
        return false;
    }
    nri::ResourceGroupDesc group = {};
    group.memoryLocation = nri::MemoryLocation::DEVICE;
    group.textures = textures.data();
    group.textureNum = (uint32_t)textures.size();
    memory_.resize(h.CalculateAllocationNumber(*context_.device, group));
    if (h.AllocateAndBindMemory(*context_.device, group, memory_.data()) != nri::Result::SUCCESS) {
        for (nri::Texture* t : textures)
            c.DestroyTexture(t);
        slots_.clear();
        memory_.clear();
        error = "cannot allocate the frame cache in video memory";
        return false;
    }
    for (GpuSlot& s : slots_) {
        nri::TextureViewDesc luma = {s.luma, nri::TextureView::TEXTURE, luma_format};
        nri::TextureViewDesc chroma = {s.chroma, nri::TextureView::TEXTURE, chroma_format};
        if (c.CreateTextureView(luma, s.luma_view) != nri::Result::SUCCESS ||
            c.CreateTextureView(chroma, s.chroma_view) != nri::Result::SUCCESS) {
            error = "cannot create texture views";
            return false;
        }
    }
    cache_.Reset((int)slots_.size());

    // Staging: upload buffers the pipe is read into, each with its own command buffer.
    staging_.resize((size_t)std::max(options_.staging_buffers, 2));
    for (Staging& st : staging_) {
        nri::BufferDesc desc = {};
        desc.size = staging_bytes;
        if (c.CreateBuffer(*context_.device, desc, st.buffer) != nri::Result::SUCCESS) {
            error = "cannot create an upload buffer";
            return false;
        }
        nri::ResourceGroupDesc staging_group = {};
        staging_group.memoryLocation = nri::MemoryLocation::HOST_UPLOAD;
        staging_group.buffers = &st.buffer;
        staging_group.bufferNum = 1;
        if (h.AllocateAndBindMemory(*context_.device, staging_group, &st.memory) != nri::Result::SUCCESS) {
            error = "cannot allocate an upload buffer";
            return false;
        }
        st.data = (uint8_t*)c.MapBuffer(*st.buffer, 0, nri::WHOLE_SIZE);
        if (!st.data) {
            error = "cannot map an upload buffer";
            return false;
        }
        if (c.CreateCommandAllocator(*context_.graphics_queue, st.allocator) != nri::Result::SUCCESS ||
            c.CreateCommandBuffer(*st.allocator, st.command_buffer) != nri::Result::SUCCESS) {
            error = "cannot create a command buffer";
            return false;
        }
    }
    const nri::FenceDesc fence_desc = {};
    if (c.CreateFence(*context_.device, fence_desc, fence_) != nri::Result::SUCCESS) {
        error = "cannot create a fence";
        return false;
    }
    return true;
}

void FfmpegSource::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
        version_++;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        while (!finished_) {
            process_.Terminate();  // ends a read the thread is blocked in (again, if it started a stream meanwhile)
            cv_.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        thread_.join();
    }
    process_.Kill();
    if (!context_.core)
        return;
    const nri::CoreInterface& c = core();
    if (fence_ && submitted_ > 0)
        c.Wait(*fence_, submitted_);
    for (Staging& st : staging_) {
        if (st.command_buffer)
            c.DestroyCommandBuffer(st.command_buffer);
        if (st.allocator)
            c.DestroyCommandAllocator(st.allocator);
        if (st.buffer)
            c.DestroyBuffer(st.buffer);
        if (st.memory)
            c.FreeMemory(st.memory);
    }
    staging_.clear();
    if (fence_)
        c.DestroyFence(fence_);
    fence_ = nullptr;
    for (GpuSlot& s : slots_) {
        if (s.luma_view)
            c.DestroyDescriptor(s.luma_view);
        if (s.chroma_view)
            c.DestroyDescriptor(s.chroma_view);
        if (s.luma)
            c.DestroyTexture(s.luma);
        if (s.chroma)
            c.DestroyTexture(s.chroma);
    }
    slots_.clear();
    for (nri::Memory* m : memory_)
        if (m)
            c.FreeMemory(m);
    memory_.clear();
}

void FfmpegSource::SetInterest(int64_t first, int64_t last, int64_t playhead, int direction) {
    const int64_t frames = info_.frames;
    playhead = std::clamp<int64_t>(playhead, 0, frames - 1);
    first = std::clamp<int64_t>(first, 0, playhead);
    last = std::clamp<int64_t>(last, playhead, frames - 1);
    // The cache holds the window, less a few slots for frames being loaded and pinned: more is trimmed on the side
    // away from the play direction.
    const int64_t room = std::max<int64_t>(4, (int64_t)slots_.size() - 6);
    int64_t ahead = last - playhead, behind = playhead - first;
    if (direction < 0)
        std::swap(ahead, behind);
    ahead = std::min(ahead, room / 2 + room / 4);
    behind = std::min(behind, std::max<int64_t>(room - 1 - ahead, 0));
    if (direction < 0)
        std::swap(ahead, behind);
    const FrameWindow w = {playhead - behind, playhead + ahead, playhead};
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        changed = w.first != window_.first || w.last != window_.last || w.playhead != window_.playhead;
        if (changed) {
            if (std::llabs(w.playhead - window_.playhead) > 1)
                jumped_at_ = Now();
            window_ = w;
            version_++;
        }
    }
    if (changed)
        cv_.notify_one();
}

DecodeStats FfmpegSource::Stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    DecodeStats s;
    double sum = 0.0;
    size_t n = 0;
    for (double t : frame_times_)
        if (t > 0.0) {
            sum += t;
            n++;
        }
    s.decode_fps = n >= 6 && sum > 0.0 ? (double)n / sum : 0.0;
    s.cached = cache_.ReadyCount();
    s.cached_bytes = (uint64_t)s.cached * planes_.frame_bytes();
    s.decoded_total = decoded_total_;
    int64_t missing = 0;
    for (int64_t f = window_.playhead; f <= window_.last; f++)
        missing += !cache_.IsReady(f);
    s.pending = missing;
    return s;
}

std::string FfmpegSource::Describe() const {
    std::lock_guard<std::mutex> lock(mu_);
    char text[256];
    std::snprintf(text, sizeof(text), "ffmpeg pipe, software decode %s %ux%u, %s%s%s", info_.codec.c_str(), info_.width,
                  info_.height, pixel_format_.c_str(), error_.empty() ? "" : "; ", error_.c_str());
    return text;
}

void FfmpegSource::RecordFrameTime(double seconds) {
    frame_times_[frame_time_next_] = seconds;
    frame_time_next_ = (frame_time_next_ + 1) % frame_times_.size();
}

bool FfmpegSource::StartStream(int64_t first_frame) {
    process_.Kill();
    std::vector<std::string> args = {ffmpeg_, "-nostdin", "-v", "error"};
    if (first_frame > 0) {
        char seek[48];
        std::snprintf(seek, sizeof(seek), "%.6f", ((double)first_frame - 0.5) / info_.fps);
        args.insert(args.end(), {"-ss", seek});
    }
    args.insert(args.end(), {"-i", path_, "-map", "0:v:0", "-an", "-sn", "-dn", "-fps_mode", "passthrough", "-f",
                             "rawvideo", "-pix_fmt", pixel_format_, "-"});
    ProcessOptions options;
    options.pipe_stdout = true;
    std::string error;
    if (!process_.Start(args, options, &error)) {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = error + " (is ffmpeg installed? set NR_FFMPEG or put it on PATH)";
        spdlog::error("{}", error_);
        return false;
    }
    return true;
}

// One frame from the pipe into a staging buffer (both planes). Rows are read in place when the staging pitch is the
// plane's; otherwise through a scratch plane.
bool FfmpegSource::ReadFrame(Staging& st) {
    auto plane = [&](uint64_t offset, uint64_t pitch, uint64_t row_bytes, uint64_t rows) {
        if (pitch == row_bytes)
            return process_.ReadFull(st.data + offset, (size_t)(row_bytes * rows));
        if (!process_.ReadFull(scratch_.data(), (size_t)(row_bytes * rows)))
            return false;
        for (uint64_t r = 0; r < rows; r++)
            std::memcpy(st.data + offset + r * pitch, scratch_.data() + r * row_bytes, (size_t)row_bytes);
        return true;
    };
    return plane(0, luma_pitch_, planes_.luma_row_bytes, planes_.luma_height) &&
           plane(chroma_offset_, chroma_pitch_, planes_.chroma_row_bytes, planes_.chroma_height);
}

void FfmpegSource::SubmitUpload(int slot_index, Staging& st) {
    const nri::CoreInterface& c = core();
    const GpuSlot& slot = slots_[(size_t)slot_index];
    c.ResetCommandAllocator(*st.allocator);
    nri::CommandBuffer& cb = *st.command_buffer;
    c.BeginCommandBuffer(cb, nullptr);
    nri::TextureBarrierDesc barriers[2] = {};
    nri::Texture* textures[2] = {slot.luma, slot.chroma};
    for (int i = 0; i < 2; i++) {
        barriers[i].texture = textures[i];
        barriers[i].before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::ALL};
        barriers[i].after = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
        barriers[i].layerNum = 1;
        barriers[i].mipNum = 1;
    }
    nri::BarrierDesc barrier = {};
    barrier.textures = barriers;
    barrier.textureNum = 2;
    c.CmdBarrier(cb, barrier);
    nri::TextureRegionDesc luma_region = {};
    luma_region.width = (nri::Dim_t)planes_.luma_width;
    luma_region.height = (nri::Dim_t)planes_.luma_height;
    luma_region.depth = 1;
    nri::TextureDataLayoutDesc luma_layout = {0, (uint32_t)luma_pitch_, (uint32_t)(luma_pitch_ * planes_.luma_height)};
    c.CmdUploadBufferToTexture(cb, *slot.luma, luma_region, *st.buffer, luma_layout);
    nri::TextureRegionDesc chroma_region = {};
    chroma_region.width = (nri::Dim_t)planes_.chroma_width;
    chroma_region.height = (nri::Dim_t)planes_.chroma_height;
    chroma_region.depth = 1;
    nri::TextureDataLayoutDesc chroma_layout = {chroma_offset_, (uint32_t)chroma_pitch_,
                                                (uint32_t)(chroma_pitch_ * planes_.chroma_height)};
    c.CmdUploadBufferToTexture(cb, *slot.chroma, chroma_region, *st.buffer, chroma_layout);
    for (int i = 0; i < 2; i++) {
        barriers[i].before = barriers[i].after;
        barriers[i].after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER};
    }
    c.CmdBarrier(cb, barrier);
    c.EndCommandBuffer(cb);
    const uint64_t value = ++submitted_;
    nri::FenceSubmitDesc signal = {fence_, value, nri::StageBits::ALL};
    nri::QueueSubmitDesc submit = {};
    submit.commandBuffers = &st.command_buffer;
    submit.commandBufferNum = 1;
    submit.signalFences = &signal;
    submit.signalFenceNum = 1;
    c.QueueSubmit(*context_.graphics_queue, submit);
    st.fence_value = value;
    std::lock_guard<std::mutex> lock(mu_);
    inflight_.push_back({slot_index, value});
}

// Marks the slots whose uploads the GPU finished as ready.
void FfmpegSource::ReapLocked() {
    const uint64_t done = core().GetFenceValue(*fence_);
    while (!inflight_.empty() && inflight_.front().value <= done) {
        cache_.MarkReady(inflight_.front().slot);
        inflight_.pop_front();
    }
}

FfmpegSource::Staging& FfmpegSource::AcquireStaging() {
    Staging& st = staging_[staging_next_];
    staging_next_ = (staging_next_ + 1) % staging_.size();
    if (st.fence_value != 0) {
        core().Wait(*fence_, st.fence_value);
        st.fence_value = 0;
        std::lock_guard<std::mutex> lock(mu_);
        ReapLocked();
    }
    return st;
}

void FfmpegSource::SettleUploads() {
    if (submitted_ > 0)
        core().Wait(*fence_, submitted_);
    std::lock_guard<std::mutex> lock(mu_);
    ReapLocked();
}

void FfmpegSource::Run() {
    int failures = 0;
    bool restarted = false;  // the next frame's time includes the stream's start
    double mark = Now();
    uint64_t seen = 0;
    std::unique_lock<std::mutex> lock(mu_);
    while (!quit_) {
        const FrameWindow window = window_;
        const int64_t need = cache_.NextMissing(window);
        const DecodePlan plan = PlanDecode(stream_next_, need, info_.frames, options_.preroll_frames, skip_limit_);
        if (failures >= 4 || plan.action == DecodePlan::Action::IDLE) {
            // Nothing to decode: let the uploads in flight finish, then wait for the window to change.
            lock.unlock();
            SettleUploads();
            lock.lock();
            if (quit_)
                break;
            if (version_ != seen) {
                seen = version_;
                continue;  // it changed while the uploads settled
            }
            cv_.wait(lock, [&] { return quit_ || version_ != seen; });
            seen = version_;
            mark = Now();
            continue;
        }
        if (plan.action == DecodePlan::Action::RESTART) {
            // A drag on the timeline moves the playhead every frame: restart the stream only once it has settled.
            if (Now() - jumped_at_ < 0.08) {
                cv_.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
            lock.unlock();
            const bool ok = StartStream(plan.restart_at);
            lock.lock();
            if (ok) {
                stream_next_ = plan.restart_at;
                restarted = true;
            } else {
                failures = 4;
            }
            seen = version_;
            continue;
        }
        // CONTINUE: the frame the stream delivers next.
        const int64_t frame = stream_next_;
        int slot = -1;
        if (window_.contains(frame) && !cache_.Present(frame)) {
            slot = cache_.Reserve(frame, window_);
            if (slot < 0) {  // no slot may be taken now: wait for an unpin or another window
                lock.unlock();
                SettleUploads();
                lock.lock();
                if (quit_)
                    break;
                const uint64_t before = version_;
                cv_.wait_for(lock, std::chrono::milliseconds(20), [&] { return quit_ || version_ != before; });
                mark = Now();
                continue;
            }
        }
        lock.unlock();
        Staging& st = AcquireStaging();
        const bool ok = !quit_ && ReadFrame(st);
        if (ok && slot >= 0)
            SubmitUpload(slot, st);
        lock.lock();
        if (!ok) {
            if (slot >= 0)
                cache_.Free(slot);
            if (quit_)
                break;
            const bool clean_end = stream_next_ >= info_.frames;
            process_.Kill();
            stream_next_ = -1;
            if (!clean_end) {
                failures++;
                error_ = "ffmpeg ended before frame " + std::to_string(frame) + " of " + std::to_string(info_.frames);
                spdlog::warn("{}: {}", path_, error_);
            }
            continue;
        }
        failures = 0;
        stream_next_ = frame + 1;
        decoded_total_++;
        const double now = Now();
        if (restarted)
            restarted = false;  // the start of a stream (the process, the keyframe) is not decode speed
        else
            RecordFrameTime(now - mark);
        mark = now;
    }
    finished_ = true;
}

}  // namespace

std::unique_ptr<VideoSource> OpenFfmpegVideo(const VideoContext& context, const std::string& path,
                                             const FfmpegOptions& options, std::string* error) {
    auto source = std::make_unique<FfmpegSource>();
    std::string why;
    if (!source->Init(context, path, options, why)) {
        if (error)
            *error = why;
        return nullptr;
    }
    return source;
}

}  // namespace renderer::clips
