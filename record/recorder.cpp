#include "record/recorder.h"

#include <windows.h>
#include <wrl/client.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

namespace record {
namespace {

using Clock = std::chrono::steady_clock;

double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

float Median(std::vector<float> v) {
    if (v.empty())
        return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

std::string JsonText(const std::string& s) {
    std::string out;
    for (char c : s)
        out += c == '\\' ? std::string("/") : c == '"' ? std::string("\\\"") : std::string(1, (unsigned char)c < 0x20 ? ' ' : c);
    return out;
}

std::string Quoted(const std::string& s) { return "\"" + JsonText(s) + "\""; }

bool MakeDirs(const std::string& path) {
    for (size_t i = 3; i <= path.size(); i++)
        if (i == path.size() || path[i] == '\\' || path[i] == '/')
            CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

double FreeGb(const std::string& dir) {
    ULARGE_INTEGER available = {};
    return GetDiskFreeSpaceExA(dir.c_str(), &available, nullptr, nullptr) ? available.QuadPart / 1e9 : -1;
}

double ThreadCpuSeconds() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    auto seconds = [](FILETIME t) { return (((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) * 1e-7; };
    return seconds(kernel) + seconds(user);
}

std::string Format(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

// D3D12's ring: slots in device memory that the packer writes, each copied to its readback buffer on the recorder's
// copy queue (after the packer's fence, where it names one).
class D3D12Ring : public Ring {
public:
    D3D12Ring(ID3D12Device* device, std::shared_ptr<Packer> packer) : device_(device), packer_(std::move(packer)) {}
    ~D3D12Ring() override {
        if (event_)
            CloseHandle(event_);
    }
    bool Describe(const void* texture, Channels& out, std::string& error) override {
        return packer_->Describe(texture, out, error);
    }
    bool Create(unsigned slots, uint64_t bytes, std::string& error) override {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_COPY};
        if (FAILED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_))) ||
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) ||
            !(event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr))) {
            error = "copy queue or fence";
            return false;
        }
        for (unsigned i = 0; i < slots; i++) {
            Slot& s = slots_[i];
            D3D12_HEAP_PROPERTIES heap = {};
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            const bool made =
                SUCCEEDED(device_->CreateCommittedResource(&heap, packer_->SharedSlots() ? D3D12_HEAP_FLAG_SHARED
                                                                                         : D3D12_HEAP_FLAG_NONE,
                                                           &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           IID_PPV_ARGS(&s.vram))) &&
                (heap.Type = D3D12_HEAP_TYPE_READBACK, desc.Flags = D3D12_RESOURCE_FLAG_NONE,
                 SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&s.readback)))) &&
                SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&s.allocator))) &&
                SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, s.allocator.Get(), nullptr,
                                                     IID_PPV_ARGS(&s.list)));
            if (!made) {
                error = "ring slot " + std::to_string(i) + " of " + std::to_string(bytes >> 20) + " MiB";
                return false;
            }
            s.list->Close();
        }
        return true;
    }
    bool Pack(void* list, uint32_t slot, uint32_t data_start, const std::vector<Packer::Texture>& textures,
              std::string& error) override {
        return packer_->Pack(list, slot, slots_[slot].vram.Get(), data_start, textures, error);
    }
    int Done(uint32_t slot, uint64_t& bytes, double& gpu_ms) override {
        Slot& s = slots_[slot];
        return packer_->Done(slot, bytes, gpu_ms, s.writer, s.writer_value);
    }
    const uint8_t* Read(uint32_t slot, uint64_t bytes, std::string& error) override {
        Slot& s = slots_[slot];
        s.allocator->Reset();
        s.list->Reset(s.allocator.Get(), nullptr);
        s.list->CopyBufferRegion(s.readback.Get(), 0, s.vram.Get(), 0, bytes);
        s.list->Close();
        ID3D12CommandList* lists[] = {s.list.Get()};
        if (s.writer)
            queue_->Wait(s.writer, s.writer_value);
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fence_value_);
        if (fence_->GetCompletedValue() < fence_value_) {
            fence_->SetEventOnCompletion(fence_value_, event_);
            WaitForSingleObject(event_, 10000);
        }
        const D3D12_RANGE range = {0, (SIZE_T)bytes};
        void* data = nullptr;
        if (FAILED(s.readback->Map(0, &range, &data)) || !data) {
            error = "RECORD: a readback buffer did not map";
            return nullptr;
        }
        return (const uint8_t*)data;
    }
    void Release(uint32_t slot) override {
        const D3D12_RANGE none = {0, 0};
        slots_[slot].readback->Unmap(0, &none);
    }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> vram, readback;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        ID3D12Fence* writer = nullptr;  // what the copy waits for, if the packer names one
        uint64_t writer_value = 0;
    };
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    std::shared_ptr<Packer> packer_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Slot slots_[MAX_SLOTS];
};

}  // namespace

std::string DefaultRoot(const std::string& configured) {
    const bool wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != nullptr;
    auto z = [](std::string unix_path) {
        for (char& c : unix_path)
            if (c == '/')
                c = '\\';
        return "Z:" + unix_path;
    };
    auto env = [](const char* name) {
        char value[MAX_PATH * 2] = {};
        const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
        return n && n < sizeof(value) ? std::string(value) : std::string();
    };
    if (!configured.empty())
        return wine && configured[0] == '/' ? z(configured) : configured;
    // Lasting folders, not caches or Temp, which cleanup tools empty.
    if (wine) {
        if (const std::string data = env("XDG_DATA_HOME"); !data.empty() && data[0] == '/')
            return z(data + "/nr-record");
        std::string home = env("WINEHOMEDIR");  // Wine's: \??\Z:\home\<user>
        if (home.rfind("\\??\\", 0) == 0)
            home = home.substr(4);
        if (!home.empty())
            return home + "\\.local\\share\\nr-record";
        if (const std::string unix_home = env("WINEHOME"); !unix_home.empty())
            return z(unix_home + "/.local/share/nr-record");
    }
    return env("LOCALAPPDATA") + "\\nr-record";
}

struct State {
    enum SlotState : int { FREE, PACKED, COPIED };  // COPIED: in memory (Ring::Read), until its blocks are done
    struct Slot {
        std::atomic<int> state{FREE};
        IndexRecord index;
    };
    struct Texture {
        InputDesc desc;
        size_t input = 0;  // its index in the inputs
        Channels c;
        uint32_t api_format = 0, table = 0;
        std::string reference;
        size_t reference_input = 0;
    };
    Config config;
    Session session;
    std::shared_ptr<Ring> ring;
    std::string dir, time;
    std::vector<Texture> textures;
    size_t input_count = 0;
    uint32_t data_start = 0;
    uint64_t slot_bytes = 0;
    size_t call_bytes = 0;
    std::vector<std::unique_ptr<Slot>> slots;
    std::FILE *frames = nullptr, *index = nullptr, *params = nullptr;
    LARGE_INTEGER qpc_frequency = {};
    // The source thread's.
    uint32_t next_slot = 0, sequence = 0, pending_dropped = 0;
    uint64_t dropped = 0, frames_seen = 0;
    Clock::time_point started = Clock::now(), stopped_at = {}, last_drop_log = {}, last_progress = Clock::now();
    std::string reason;  // why it stopped
    // Shared with the copier and writer threads.
    std::atomic<bool> stopping{false}, copier_done{false}, writer_done{false};
    std::atomic<uint64_t> written_frames{0}, written_bytes{0}, lost{0};
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<uint8_t> calls;      // params.bin bytes not yet written
    std::string requested_stop;      // the writer's (free space, a failed write)
    std::vector<float> pack_ms, copy_ms;
    double write_seconds = 0;
    // The writer's pipeline: the copier makes each copied slot a Frame of blocks and queues a Job per block; a worker
    // compresses (or with `store` copies) its block into a pool buffer, and the frame's last block frees the slot.
    // The writer writes the frames in recording order and gives their buffers back. A frame is stored without
    // compression when the workers are a frame's blocks behind, so slots come free as fast as without compression.
    struct Frame {
        IndexRecord index;
        uint32_t slot = 0;
        bool store = false;
        const uint8_t* source = nullptr;  // the slot in memory (Ring::Read)
        std::vector<uint8_t*> buffers;
        std::vector<uint32_t> sizes;      // stored bytes per block, BLOCK_STORED if not compressed
        std::atomic<uint32_t> remaining{0};
        std::atomic<uint64_t> busy_us{0};
    };
    struct Job {
        std::shared_ptr<Frame> frame;
        uint32_t block = 0;
    };
    int level = 0;
    unsigned threads = 0;
    std::mutex jobs_mutex;
    std::condition_variable jobs_cv;
    std::deque<Job> jobs;
    bool jobs_stop = false;
    std::atomic<unsigned> workers_running{0};
    std::mutex pool_mutex;
    std::condition_variable pool_cv;
    std::vector<std::unique_ptr<uint8_t[]>> pool;  // every buffer made, ZSTD_compressBound(BLOCK_BYTES) bytes each
    std::vector<uint8_t*> pool_free;
    size_t pool_cap = 0, pool_peak = 0;
    std::deque<std::shared_ptr<Frame>> pending;    // mutex: frames in recording order, for the writer
    std::atomic<uint64_t> raw_bytes{0}, stored_frames{0};
    std::atomic<bool> queue_full{false};  // the copier waits for memory: the disk is behind
    std::string drop_cause;                // the source thread's: why the ring was last full
    double compress_cpu_s = 0, writer_cpu_s = 0, copier_cpu_s = 0;  // mutex

    void Log(Level level, const std::string& line) const {
        if (config.log)
            config.log(level, line);
    }
    void Notify() { cv.notify_all(); }
    void RequestStop(const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex);
        if (requested_stop.empty())
            requested_stop = why;
    }
};

namespace {

void WriteRecordJson(State& r, const std::string& status) {
    const Session& s = r.session;
    std::string j = "{\"format\": \"nr-record-" + std::to_string(FORMAT_VERSION) + "\", \"coding\": " +
                    std::to_string(CODING_VERSION) + ", \"feature\": " + Quoted(s.feature) + ", \"source\": " +
                    Quoted(s.source) + ", \"graphics_api\": " + Quoted(s.graphics_api) + ", \"recorder\": " +
                    Quoted(s.recorder) + ", \"game\": " + Quoted(s.game) + ", \"time\": " + Quoted(r.time) +
                    ", \"width\": " + std::to_string(s.width) + ", \"height\": " + std::to_string(s.height);
    for (const auto& [key, value] : s.fields)
        j += ", " + Quoted(key) + ": " + value;
    j += ", \"qpc_frequency\": " + std::to_string(r.qpc_frequency.QuadPart) + ", \"data_start\": " +
         std::to_string(r.data_start) + ", \"slot_bytes\": " + std::to_string(r.slot_bytes) + ", \"ring\": " +
         std::to_string(r.slots.size()) + ", \"textures\": [";
    const bool d3d12 = s.graphics_api == "d3d12";
    for (size_t i = 0; i < r.textures.size(); i++) {
        const State::Texture& t = r.textures[i];
        j += std::string(i ? ", " : "") + "{\"role\": " + Quoted(t.desc.role) + ", \"name\": " + Quoted(t.desc.role) +
             ", \"api_format\": " + std::to_string(t.api_format) +
             (d3d12 ? ", \"dxgi_format\": " + std::to_string(t.api_format) : std::string()) +
             ", \"vk_format\": " + std::to_string(t.c.vk_format) + ", \"width\": " + std::to_string(t.c.width) +
             ", \"height\": " + std::to_string(t.c.height) + ", \"channels\": " + std::to_string(t.c.channels) +
             ", \"kinds\": " + std::to_string(t.c.kinds) + ", \"depth\": " + (t.c.depth ? "true" : "false") +
             ", \"srgb\": " + (t.c.srgb ? "true" : "false") + ", \"reference\": " +
             (t.reference.empty() ? "null" : Quoted(t.reference)) + ", \"table\": " + std::to_string(t.table) + "}";
    }
    j += "], \"parameter_source\": " + Quoted(s.parameter_source) + ", \"parameters\": [";
    for (size_t i = 0; i < s.parameters.size(); i++)
        j += std::string(i ? ", " : "") + Quoted(s.parameters[i]);
    j += "], \"matrices\": [";
    for (size_t i = 0; i < s.matrices.size(); i++)
        j += std::string(i ? ", " : "") + Quoted(s.matrices[i]);
    std::vector<float> pack, copy;
    double seconds = 0, compress_cpu = 0, writer_cpu = 0, copier_cpu = 0;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        pack = r.pack_ms;
        copy = r.copy_ms;
        seconds = r.write_seconds;
        compress_cpu = r.compress_cpu_s;
        writer_cpu = r.writer_cpu_s;
        copier_cpu = r.copier_cpu_s;
    }
    size_t peak = 0;
    {
        std::lock_guard<std::mutex> lock(r.pool_mutex);
        peak = r.pool_peak;
    }
    j += Format("], \"call_bytes\": %zu, \"index_bytes\": %zu, \"zstd_level\": %d, \"block_bytes\": %u, "
                "\"threads\": %u, \"queue_mb\": %u, \"status\": \"%s\", \"frames\": %llu, \"dropped\": %llu, "
                "\"lost\": %llu, \"bytes\": %llu, \"raw_bytes\": %llu, \"stored_frames\": %llu, "
                "\"queue_peak_mb\": %.0f, \"pack_gpu_ms_median\": %.4f, \"copy_ms_median\": %.4f, "
                "\"write_seconds\": %.3f, \"compress_cpu_seconds\": %.3f, \"writer_cpu_seconds\": %.3f, "
                "\"copier_cpu_seconds\": %.3f}\n",
                r.call_bytes, sizeof(IndexRecord), r.level, BLOCK_BYTES, r.threads, r.config.queue_mb,
                JsonText(status).c_str(), (unsigned long long)r.written_frames.load(),
                (unsigned long long)r.dropped, (unsigned long long)r.lost.load(),
                (unsigned long long)r.written_bytes.load(), (unsigned long long)r.raw_bytes.load(),
                (unsigned long long)r.stored_frames.load(), peak * (double)ZSTD_compressBound(BLOCK_BYTES) / 1048576,
                Median(pack), Median(copy), seconds, compress_cpu, writer_cpu, copier_cpu);
    std::FILE* f = std::fopen((r.dir + "\\record.json.part").c_str(), "wb");
    if (f) {
        std::fwrite(j.data(), 1, j.size(), f);
        std::fclose(f);
    }
    MoveFileExA((r.dir + "\\record.json.part").c_str(), (r.dir + "\\record.json").c_str(), MOVEFILE_REPLACE_EXISTING);
}

// n pool buffers for a frame's blocks: made up to pool_cap, then waits for the writer to give some back.
std::vector<uint8_t*> TakeBuffers(State& r, size_t n) {
    const size_t bound = ZSTD_compressBound(BLOCK_BYTES);
    std::unique_lock<std::mutex> lock(r.pool_mutex);
    while (r.pool_free.size() < n) {
        if (r.pool.size() < r.pool_cap) {
            r.pool.emplace_back(new uint8_t[bound]);
            r.pool_free.push_back(r.pool.back().get());
            continue;
        }
        r.queue_full = true;
        r.pool_cv.wait_for(lock, std::chrono::milliseconds(20));
    }
    r.queue_full = false;
    std::vector<uint8_t*> out(r.pool_free.end() - n, r.pool_free.end());
    r.pool_free.resize(r.pool_free.size() - n);
    r.pool_peak = std::max(r.pool_peak, r.pool.size() - r.pool_free.size());
    return out;
}

// A slot in memory (Ring::Read) as a frame for the workers and the writer.
void SubmitFrame(State& r, uint32_t slot, const uint8_t* mapped) {
    State::Slot& s = *r.slots[slot];
    auto f = std::make_shared<State::Frame>();
    f->index = s.index;
    f->index.raw_bytes = (uint32_t)s.index.bytes;
    f->slot = slot;
    f->source = mapped;
    const uint32_t blocks = (uint32_t)((s.index.bytes + BLOCK_BYTES - 1) / BLOCK_BYTES);
    f->buffers = TakeBuffers(r, blocks);
    f->sizes.assign(blocks, 0);
    f->remaining = blocks;
    {
        std::lock_guard<std::mutex> lock(r.jobs_mutex);
        f->store = r.level == 0 || r.jobs.size() >= blocks;
        for (uint32_t b = 0; b < blocks; b++) {
            if (f->store)  // ahead of the waiting compressions, in block order
                r.jobs.push_front({f, blocks - 1 - b});
            else
                r.jobs.push_back({f, b});
        }
    }
    if (f->store && r.level)
        r.stored_frames++;
    r.jobs_cv.notify_all();
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.pending.push_back(f);
    }
    r.Notify();
}

// The copier: each packed slot, in ring order, once its pack has finished on the GPU, into memory (the ring's Read),
// then to the workers. While stopping it waits at most 2 s in all for packs that never ran
// (a list the source dropped); their slots are freed as lost.
void Copier(std::shared_ptr<State> r) {
    uint32_t next = 0;
    Clock::time_point deadline = Clock::time_point::max();
    for (;;) {
        State::Slot& s = *r->slots[next];
        if (s.state.load() != State::PACKED) {
            if (r->stopping)
                break;
            std::unique_lock<std::mutex> lock(r->mutex);
            r->cv.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }
        uint64_t bytes = 0;
        double gpu_ms = 0;
        int done = 0;
        while ((done = r->ring->Done(next, bytes, gpu_ms)) == 0) {
            if (r->stopping && deadline == Clock::time_point::max())
                deadline = Clock::now() + std::chrono::seconds(2);
            if (Clock::now() > deadline)
                break;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (done != 1 || bytes > r->slot_bytes || bytes < 4ull * r->data_start) {
            r->lost++;
            s.state = State::FREE;
            r->Notify();
            next = (next + 1) % r->slots.size();
            if (Clock::now() > deadline)
                break;
            continue;
        }
        const auto start = Clock::now();
        std::string why;
        const uint8_t* data = r->ring->Read(next, bytes, why);
        s.index.bytes = bytes;
        s.index.pack_gpu_ms = (float)gpu_ms;
        s.index.copy_ms = (float)Ms(start, Clock::now());
        if (!data) {
            r->lost++;
            r->RequestStop(why);
            s.state = State::FREE;
        } else {
            s.state = State::COPIED;
            SubmitFrame(*r, next, data);
        }
        next = (next + 1) % r->slots.size();
    }
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        r->copier_cpu_s = ThreadCpuSeconds();
    }
    r->copier_done = true;
    r->Notify();
}

// A worker: blocks of the queued frames, zstd at the recording's level (or copied: `store`, or a block zstd does not
// make smaller); the frame's last block releases the slot's memory (Ring::Release) and frees it.
void Worker(std::shared_ptr<State> r) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ZSTD_CCtx* cctx = r->level ? ZSTD_createCCtx() : nullptr;
    const size_t bound = ZSTD_compressBound(BLOCK_BYTES);
    for (;;) {
        State::Job job;
        {
            std::unique_lock<std::mutex> lock(r->jobs_mutex);
            r->jobs_cv.wait(lock, [&] { return r->jobs_stop || !r->jobs.empty(); });
            if (r->jobs.empty())
                break;
            job = std::move(r->jobs.front());
            r->jobs.pop_front();
        }
        State::Frame& f = *job.frame;
        const auto start = Clock::now();
        const uint64_t at = (uint64_t)job.block * BLOCK_BYTES;
        const size_t n = (size_t)std::min<uint64_t>(BLOCK_BYTES, f.index.raw_bytes - at);
        uint8_t* out = f.buffers[job.block];
        size_t size = 0;
        if (!f.store && cctx) {
            size = ZSTD_compressCCtx(cctx, out, bound, f.source + at, n, r->level);
            if (ZSTD_isError(size) || size >= n)
                size = 0;
        }
        if (!size) {
            std::memcpy(out, f.source + at, n);
            size = n | BLOCK_STORED;
        }
        f.sizes[job.block] = (uint32_t)size;
        f.busy_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        if (--f.remaining == 0) {
            r->ring->Release(f.slot);
            r->slots[f.slot]->state = State::FREE;
            r->Notify();
        }
    }
    if (cctx)
        ZSTD_freeCCtx(cctx);
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        r->compress_cpu_s += ThreadCpuSeconds();
    }
    r->workers_running--;
    r->Notify();
}

// The writer: params.bin as calls arrive, each frame once its blocks are done, in recording order, into frames.bin
// and frames.idx; the free space every half second. Then it stops the workers and writes record.json.
void Writer(std::shared_ptr<State> r) {
    uint64_t offset = 0;
    auto last_space = Clock::now();
    bool failed = false;
    for (;;) {
        std::vector<uint8_t> calls;
        std::shared_ptr<State::Frame> f;
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(r->mutex);
            calls.swap(r->calls);
            if (!r->pending.empty() && r->pending.front()->remaining.load() == 0) {
                f = r->pending.front();
                r->pending.pop_front();
            }
            finished = !f && r->copier_done && r->pending.empty();
        }
        if (!calls.empty() && std::fwrite(calls.data(), 1, calls.size(), r->params) != calls.size() && !failed) {
            failed = true;
            r->RequestStop("params.bin: write failed");
        }
        if (f) {
            const auto start = Clock::now();
            const uint32_t blocks = (uint32_t)f->sizes.size();
            std::vector<uint8_t> head(sizeof(BlocksHeader) + 4ull * blocks);
            BlocksHeader h;
            h.blocks = blocks;
            h.raw_bytes = f->index.raw_bytes;
            h.block_bytes = BLOCK_BYTES;
            h.level = r->level;
            std::memcpy(head.data(), &h, sizeof(h));
            std::memcpy(head.data() + sizeof(h), f->sizes.data(), 4ull * blocks);
            uint64_t bytes = head.size();
            bool ok = !failed && std::fwrite(head.data(), 1, head.size(), r->frames) == head.size();
            for (uint32_t b = 0; b < blocks && ok; b++) {
                const size_t n = f->sizes[b] & ~BLOCK_STORED;
                ok = std::fwrite(f->buffers[b], 1, n, r->frames) == n;
                bytes += n;
            }
            f->index.offset = offset;
            f->index.bytes = bytes;
            f->index.compress_ms = (float)(f->busy_us.load() / 1000.0);
            ok = ok && std::fwrite(&f->index, sizeof(IndexRecord), 1, r->index) == 1;
            if (ok) {
                offset += bytes;
                r->written_frames++;
                r->written_bytes += bytes;
                r->raw_bytes += f->index.raw_bytes;
            } else if (!failed) {
                failed = true;
                r->RequestStop("frames.bin: write failed (disk full?)");
            }
            {
                std::lock_guard<std::mutex> lock(r->pool_mutex);
                r->pool_free.insert(r->pool_free.end(), f->buffers.begin(), f->buffers.end());
            }
            r->pool_cv.notify_all();
            std::lock_guard<std::mutex> lock(r->mutex);
            r->pack_ms.push_back(f->index.pack_gpu_ms);
            r->copy_ms.push_back(f->index.copy_ms);
            r->write_seconds += Ms(start, Clock::now()) / 1000;
            continue;
        }
        if (finished)
            break;
        if (Clock::now() - last_space > std::chrono::milliseconds(500)) {
            last_space = Clock::now();
            const double free_gb = FreeGb(r->dir);
            if (free_gb >= 0 && free_gb < r->config.free_gb)
                r->RequestStop(Format("free space %.1f GB below record_free_gb %.0f", free_gb, r->config.free_gb));
        }
        std::unique_lock<std::mutex> lock(r->mutex);
        r->cv.wait_for(lock, std::chrono::milliseconds(5));
    }
    {
        std::lock_guard<std::mutex> lock(r->jobs_mutex);
        r->jobs_stop = true;
    }
    r->jobs_cv.notify_all();
    for (int i = 0; i < 400 && r->workers_running.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        r->writer_cpu_s = ThreadCpuSeconds();
    }
    std::fclose(r->frames);
    std::fclose(r->index);
    std::fclose(r->params);
    WriteRecordJson(*r, r->reason.empty() ? "complete" : "stopped: " + r->reason);
    r->writer_done = true;
}

// The stop from any of the source thread's calls: the threads write what was packed, then record.json.
void StopNow(State& r, const std::string& reason) {
    if (!r.stopping) {
        r.reason = reason;
        r.stopping = true;
    }
    if (r.stopped_at == Clock::time_point{})
        r.stopped_at = Clock::now();
    r.Notify();
}

// Why a frame's inputs no longer match the recording's textures (presence, size, format), or empty.
std::string Mismatch(const State& r, const std::vector<Input>& inputs) {
    if (inputs.size() != r.input_count)
        return "the inputs changed";
    for (const State::Texture& t : r.textures) {
        const Input& in = inputs[t.input];
        if (!in.texture || in.width != t.c.width || in.height != t.c.height || in.api_format != t.api_format)
            return t.desc.role + " changed (size, format or presence)";
    }
    return {};
}

}  // namespace

std::unique_ptr<Recorder> Recorder::Start(ID3D12Device* device, std::shared_ptr<Packer> packer,
                                          const Config& config, const Session& session,
                                          const std::vector<InputDesc>& inputs, const std::vector<Input>& first,
                                          std::string& error) {
    if (!device || !packer) {
        error = "no device or packer";
        return nullptr;
    }
    return Start(std::make_shared<D3D12Ring>(device, std::move(packer)), config, session, inputs, first, error);
}

std::unique_ptr<Recorder> Recorder::Start(std::shared_ptr<Ring> ring, const Config& config, const Session& session,
                                          const std::vector<InputDesc>& inputs, const std::vector<Input>& first,
                                          std::string& error) {
    if (!ring || inputs.size() != first.size()) {
        error = "no ring or inputs";
        return nullptr;
    }
    auto r = std::make_shared<State>();
    r->config = config;
    r->config.ring = std::clamp(config.ring, 2u, MAX_SLOTS);
    r->session = session;
    r->ring = std::move(ring);
    r->input_count = inputs.size();
    r->call_bytes = CallBytes(session.parameters.size(), session.matrices.size());
    uint32_t table = 1;
    uint64_t worst = 0;
    for (size_t i = 0; i < inputs.size(); i++) {
        if (!first[i].texture) {
            if (inputs[i].required) {
                error = inputs[i].role + ": not given";
                return nullptr;
            }
            continue;
        }
        State::Texture t;
        t.desc = inputs[i];
        t.input = i;
        t.api_format = first[i].api_format;
        std::string why;
        if (!r->ring->Describe(first[i].texture, t.c, why)) {
            if (inputs[i].required) {
                error = inputs[i].role + ": " + why;
                return nullptr;
            }
            r->Log(Level::Warn, "RECORD leaves out " + inputs[i].role + ": " + why);
            continue;
        }
        t.table = table;
        table += (uint32_t)TableWords(t.c);
        worst += WorstWords(t.c);
        r->textures.push_back(t);
    }
    for (State::Texture& t : r->textures)  // a texture as its difference from its reference role's
        if (!t.desc.reference.empty())
            for (const State::Texture& h : r->textures)
                if (h.desc.role == t.desc.reference && h.c.vk_format == t.c.vk_format && h.c.width == t.c.width &&
                    h.c.height == t.c.height) {
                    t.reference = h.desc.role;
                    t.reference_input = h.input;
                }
    r->data_start = (table + 63) / 64 * 64;
    r->slot_bytes = 4 * (r->data_start + worst);
    char stamp[32], iso[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    std::string game = session.game;  // the folder: date, time and the game's executable without .exe
    game = game.substr(game.find_last_of("\\/") + 1);
    game = game.substr(0, game.rfind('.'));
    for (char& c : game)
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_')
            c = '-';
    r->dir = DefaultRoot(config.root) + "\\" + stamp + (game.empty() ? "" : "-" + game) +
             (config.tag.empty() ? "" : "-" + config.tag);
    r->time = iso;
    if (!MakeDirs(r->dir)) {
        error = "cannot create " + r->dir;
        return nullptr;
    }
    if (const double space = FreeGb(r->dir); space >= 0 && space < config.free_gb + 1) {
        error = "free space " + std::to_string((int)space) + " GB is below record_free_gb";
        RemoveDirectoryA(r->dir.c_str());
        return nullptr;
    }
    if (!r->ring->Create(r->config.ring, r->slot_bytes, error)) {
        RemoveDirectoryA(r->dir.c_str());
        return nullptr;
    }
    for (unsigned i = 0; i < r->config.ring; i++)
        r->slots.push_back(std::make_unique<State::Slot>());
    r->frames = std::fopen((r->dir + "\\frames.bin").c_str(), "wb");
    r->index = std::fopen((r->dir + "\\frames.idx").c_str(), "wb");
    r->params = std::fopen((r->dir + "\\params.bin").c_str(), "wb");
    if (!r->frames || !r->index || !r->params) {
        error = "cannot write in " + r->dir;
        for (std::FILE* file : {r->frames, r->index, r->params})
            if (file)
                std::fclose(file);
        return nullptr;
    }
    std::setvbuf(r->frames, nullptr, _IONBF, 0);  // the writer writes whole blocks
    r->level = config.zstd_level;
    r->threads = config.threads ? config.threads : std::clamp(std::thread::hardware_concurrency() / 4, 2u, 8u);
    r->pool_cap = std::max<size_t>((size_t)config.queue_mb * 1048576 / ZSTD_compressBound(BLOCK_BYTES),
                                   r->slot_bytes / BLOCK_BYTES + 2);
    QueryPerformanceFrequency(&r->qpc_frequency);
    WriteRecordJson(*r, "recording");
    r->workers_running = r->threads;
    for (unsigned i = 0; i < r->threads; i++)
        std::thread([r] { Worker(r); }).detach();
    std::thread([r] { Copier(r); }).detach();
    std::thread([r] { Writer(r); }).detach();
    auto out = std::unique_ptr<Recorder>(new Recorder());
    out->s_ = std::move(r);
    return out;
}

Recorder::~Recorder() {
    if (s_ && !s_->stopping) {
        if (s_->reason.empty())
            s_->reason = "closed";
        s_->stopping = true;
    }
    if (s_)
        s_->Notify();
}

void Recorder::Frame(void* list, uint64_t frame_number, const std::vector<Input>& inputs) {
    State& r = *s_;
    if (r.stopping)
        return;
    if (Clock::now() - r.last_progress > std::chrono::seconds(10)) {
        r.last_progress = Clock::now();
        r.Log(Level::Info, Progress());
    }
    r.frames_seen++;
    State::Slot& s = *r.slots[r.next_slot];
    if (s.state.load() != State::FREE) {
        r.dropped++;
        r.pending_dropped++;
        bool compressing = false;
        if (r.level) {
            std::lock_guard<std::mutex> lock(r.jobs_mutex);
            compressing = !r.jobs.empty();
        }
        r.drop_cause = r.queue_full ? "the disk was behind" : compressing ? "compression was behind"
                                                                         : "the GPU's packs or copies were behind";
        if (Clock::now() - r.last_drop_log > std::chrono::seconds(1)) {
            r.last_drop_log = Clock::now();
            const double seconds = std::max(1e-3, Ms(r.started, Clock::now()) / 1000);
            r.Log(Level::Warn, Format("RECORD: ring full, %llu real frames dropped so far; %s", (unsigned long long)r.dropped,
                                      r.drop_cause.c_str()) +
                                   (r.queue_full ? Format(" (%u MB of frames wait for it, %.0f MB/s written)",
                                                          r.config.queue_mb, r.written_bytes / 1e6 / seconds)
                                                 : std::string()));
        }
        return;
    }
    if (const std::string why = Mismatch(r, inputs); !why.empty())
        return StopNow(r, why);
    std::vector<Packer::Texture> textures;
    for (const State::Texture& t : r.textures) {
        Packer::Texture pt;
        pt.texture = inputs[t.input].texture;
        pt.table = t.table;
        if (!t.reference.empty())
            pt.reference = inputs[t.reference_input].texture;
        textures.push_back(pt);
    }
    s.index = {};
    s.index.sequence = r.sequence;
    s.index.frame = frame_number;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    s.index.qpc = qpc.QuadPart;
    s.index.dropped = r.pending_dropped;
    std::string error;
    if (!r.ring->Pack(list, r.next_slot, r.data_start, textures, error))
        return StopNow(r, "pack failed: " + error);
    s.state = State::PACKED;
    r.Notify();
    r.sequence++;
    r.pending_dropped = 0;
    r.next_slot = (r.next_slot + 1) % r.slots.size();
}

void Recorder::Call(const CallHeader& header, const uint8_t* values) {
    State& r = *s_;
    if (r.stopping)
        return;
    std::lock_guard<std::mutex> lock(r.mutex);
    const size_t at = r.calls.size();
    r.calls.resize(at + r.call_bytes);
    std::memcpy(r.calls.data() + at, &header, sizeof(header));
    std::memcpy(r.calls.data() + at + sizeof(header), values, r.call_bytes - sizeof(header));
}

std::string Recorder::Due() const {
    const State& r = *s_;
    if (r.stopping)
        return {};
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        if (!r.requested_stop.empty())
            return r.requested_stop;
    }
    if (r.config.max_seconds && Ms(r.started, Clock::now()) > 1000.0 * r.config.max_seconds)
        return "record_max_seconds reached";
    if (r.config.max_frames && r.frames_seen >= r.config.max_frames)
        return "record_frames reached";
    return {};
}

void Recorder::Stop(const std::string& reason) { StopNow(*s_, reason); }

bool Recorder::StopAndWait(const std::string& reason, unsigned ms) {
    Stop(reason);
    for (unsigned i = 0; i < ms / 10 && !s_->writer_done; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return s_->writer_done;
}

bool Recorder::stopping() const { return s_->stopping; }
bool Recorder::finished() const { return s_->writer_done; }
const std::string& Recorder::dir() const { return s_->dir; }
const std::string& Recorder::reason() const { return s_->reason; }
unsigned Recorder::slots() const { return (unsigned)s_->slots.size(); }
double Recorder::slot_mib() const { return s_->slot_bytes / 1048576.0; }
unsigned Recorder::threads() const { return s_->threads; }
int Recorder::zstd_level() const { return s_->level; }

std::string Recorder::Layout() const {
    std::string layout;
    for (const State::Texture& t : s_->textures)
        layout += (layout.empty() ? "" : ", ") + t.desc.role + " " + std::to_string(t.c.width) + "x" +
                  std::to_string(t.c.height) + " vk " + std::to_string(t.c.vk_format) +
                  (t.reference.empty() ? "" : " (difference from " + t.reference + ")");
    return layout;
}

std::string Recorder::Status() const {
    const State& r = *s_;
    std::string mode;
    for (const auto& [key, value] : r.session.fields)
        if (key == "mode")
            mode = value.size() >= 2 && value.front() == '"' ? value.substr(1, value.size() - 2) : value;
    return Format("RECORD %s: %.0f s, %llu frames, %.1f GB, %llu dropped", mode.empty() ? r.session.feature.c_str()
                                                                                         : mode.c_str(),
                  Ms(r.started, Clock::now()) / 1000, (unsigned long long)r.written_frames.load(),
                  r.written_bytes.load() / 1e9, (unsigned long long)r.dropped);
}

std::string Recorder::Progress() const {
    State& r = *s_;
    std::vector<float> pack, copy;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        pack = r.pack_ms;
        copy = r.copy_ms;
    }
    const double seconds = std::max(1e-3, Ms(r.started, Clock::now()) / 1000);
    const std::string zstd = r.level ? Format(" for %.0f MB/s coded (zstd %d, %llu frames stored for speed)",
                                              r.raw_bytes / 1e6 / seconds, r.level,
                                              (unsigned long long)r.stored_frames.load())
                                     : std::string();
    return Status() + Format("; %.0f MB/s written", r.written_bytes / 1e6 / seconds) + zstd +
           Format("; pack GPU ms median %.3f, copy ms median %.3f", Median(pack), Median(copy));
}

std::string Recorder::Summary(bool with_reason) const {
    const State& r = *s_;
    const Clock::time_point end = r.stopped_at == Clock::time_point{} ? Clock::now() : r.stopped_at;
    return Format("RECORD stopped%s: %.1f s, %llu frames, %.2f GB, %llu dropped%s; %s",
                  with_reason ? (" (" + r.reason + ")").c_str() : "", Ms(r.started, end) / 1000,
                  (unsigned long long)r.written_frames.load(), r.written_bytes.load() / 1e9,
                  (unsigned long long)r.dropped, r.dropped ? (" (" + r.drop_cause + ")").c_str() : "",
                  r.dir.substr(r.dir.find_last_of('\\') + 1).c_str());
}

std::string Recorder::Totals() const {
    const State& r = *s_;
    return Format("%llu real frames, %llu dropped%s, %llu lost, %.2f GB%s", (unsigned long long)r.written_frames.load(),
                  (unsigned long long)r.dropped, r.dropped ? (" (" + r.drop_cause + ")").c_str() : "",
                  (unsigned long long)r.lost.load(), r.written_bytes / 1e9,
                  r.level ? Format(" (%.2f GB coded, zstd %d)", r.raw_bytes / 1e9, r.level).c_str() : "");
}

}  // namespace record
