// nr-replay (README.md): runs a frame generation implementation on an nr-record FG recording. Each recorded real
// frame's textures are decoded, uploaded to D3D12 textures of their recorded formats, and given to the generator with
// each call's recorded parameters; its generated frames are read back and written as raw frames and/or a frame stream,
// with replay.json describing every output frame.
//
//   nr-replay --record DIR --out DIR --generator provider|nvidia --dll PATH [--label TEXT] [--frames A:B]
//       [--multiplier M] [--mode interleaved|generated] [--raw] [--stream PATH|-] [--ngx-log]

#include <windows.h>

#include <d3d12.h>
#include <io.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "record/reader.h"
#include "record/replay/frames.h"
#include "record/replay/generator.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr const char* USAGE =
    "usage: nr-replay --record DIR --out DIR --generator provider|nvidia --dll PATH [--label TEXT]\n"
    "                 [--frames A:B] [--multiplier M] [--mode interleaved|generated] [--raw] [--stream PATH|-]\n"
    "                 [--ngx-log]\n";

// A bounded queue between threads. Pop is false once it is closed and empty.
template <typename T>
class Channel {
public:
    explicit Channel(size_t capacity) : capacity_(capacity) {}
    bool Push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_)
            return false;
        items_.push_back(std::move(value));
        changed_.notify_all();
        return true;
    }
    bool Pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closed_ || !items_.empty(); });
        if (items_.empty())
            return false;
        value = std::move(items_.front());
        items_.pop_front();
        changed_.notify_all();
        return true;
    }
    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<T> items_;
    size_t capacity_;
    bool closed_ = false;
};

// The DXGI format of a recorded texture: D3D12 recordings give it; Vulkan recordings' formats map to their equivalents.
DXGI_FORMAT DxgiOf(const record::Recording& rec, const record::Texture& t) {
    if (rec.graphics_api != "vulkan")
        return (DXGI_FORMAT)t.api_format;
    switch (t.vk) {
    case 37: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 43: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 44: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 50: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case 64: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 83: return DXGI_FORMAT_R16G16_FLOAT;
    case 97: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 100: return DXGI_FORMAT_R32_FLOAT;
    case 103: return DXGI_FORMAT_R32G32_FLOAT;
    case 126: return DXGI_FORMAT_D32_FLOAT;
    case 130: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool DepthFamily(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_D32_FLOAT ||
           f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D16_UNORM;
}

// The writer's end of the frame stream: a file or FIFO, or standard output.
class StreamWriter {
public:
    ~StreamWriter() {
        if (handle_ != INVALID_HANDLE_VALUE && own_)
            CloseHandle(handle_);
    }
    bool Open(const std::string& path, std::string& error) {
        if (path == "-") {
            // The stream keeps its own handle of standard output; what else prints there (a generator's DLL) goes to
            // standard error, out of the stream.
            if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(), &handle_,
                                 0, FALSE, DUPLICATE_SAME_ACCESS))
                handle_ = INVALID_HANDLE_VALUE;
            own_ = true;
            std::fflush(stdout);
            _dup2(_fileno(stderr), _fileno(stdout));
            SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
        } else {
            handle_ = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
            own_ = true;
            if (handle_ != INVALID_HANDLE_VALUE && GetFileType(handle_) == FILE_TYPE_DISK)
                SetEndOfFile(handle_);  // a regular file starts empty; a FIFO or pipe has no end to set
        }
        if (handle_ == INVALID_HANDLE_VALUE || !handle_) {
            error = "cannot open the stream " + path + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    }
    bool Write(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        while (n) {
            DWORD written = 0;
            const DWORD chunk = (DWORD)std::min<size_t>(n, 16u << 20);
            if (!WriteFile(handle_, p, chunk, &written, nullptr) || !written)
                return false;
            p += written;
            n -= written;
        }
        return true;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool own_ = false;
};

struct Options {
    std::string record, out, generator, dll, label, mode, stream;
    int64_t first = 0, last = -1;  // recorded real frames [first, last); -1: to the end
    uint32_t multiplier = 0;       // 0: the recorded one
    bool raw = false, ngx_log = false;
};

bool Parse(int argc, char** argv, Options& o, std::string& error) {
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        if (k == "--raw") {
            o.raw = true;
            continue;
        }
        if (k == "--ngx-log") {
            o.ngx_log = true;
            continue;
        }
        if (i + 1 >= argc) {
            error = k + " needs a value";
            return false;
        }
        const std::string v = argv[++i];
        if (k == "--record") o.record = v;
        else if (k == "--out") o.out = v;
        else if (k == "--generator") o.generator = v;
        else if (k == "--dll") o.dll = v;
        else if (k == "--label") o.label = v;
        else if (k == "--mode") o.mode = v;
        else if (k == "--stream") o.stream = v;
        else if (k == "--multiplier") o.multiplier = (uint32_t)std::stoul(v);
        else if (k == "--frames") {
            const size_t colon = v.find(':');
            if (colon == std::string::npos) {
                error = "--frames is A:B (recorded real frames A to B - 1; A: and :B leave an end open)";
                return false;
            }
            o.first = colon ? std::stoll(v.substr(0, colon)) : 0;
            o.last = colon + 1 < v.size() ? std::stoll(v.substr(colon + 1)) : -1;
        } else {
            error = "unknown option " + k;
            return false;
        }
    }
    if (o.record.empty() || o.out.empty() || o.generator.empty() || o.dll.empty()) {
        error = "--record, --out, --generator and --dll are needed";
        return false;
    }
    if (!o.raw && o.stream.empty()) {
        error = "nothing to write: give --raw (raw frames in --out) and/or --stream PATH|-";
        return false;
    }
    return true;
}

// The video rate at the recorded pace: the median interval between consecutive replayed real frames with nothing
// dropped between them, times the output frames per real frame; whole within 2% is taken whole.
double FrameRate(const record::Recording& rec, size_t first, size_t last, uint32_t per_real) {
    const double frequency = rec.meta.value("qpc_frequency", 0.0);
    std::vector<int64_t> intervals;
    for (size_t k = first + 1; k < last; k++)
        if (!rec.index[k].dropped && rec.index[k].qpc > rec.index[k - 1].qpc)
            intervals.push_back(rec.index[k].qpc - rec.index[k - 1].qpc);
    if (intervals.empty() || frequency <= 0)
        return 0;
    std::nth_element(intervals.begin(), intervals.begin() + intervals.size() / 2, intervals.end());
    const double fps = frequency / (double)intervals[intervals.size() / 2] * per_real;
    return std::abs(fps - std::round(fps)) <= 0.02 * fps ? std::round(fps) : fps;
}

struct Decoded {
    size_t k = 0;
    std::map<std::string, std::vector<uint8_t>> textures;
    std::string error;
};

struct OutFrame {
    replay::StreamFrame header;
    std::string stem;
    std::vector<uint8_t> texels;
};

int Run(const Options& o, nlohmann::json& report, std::string& error) {
    record::Recording rec;
    if (!record::Open(o.record, rec, error))
        return 1;
    if (rec.feature != "fg") {
        error = "the recording is " + rec.feature + ", not frame generation";
        return 1;
    }
    if (rec.parameter_source != "ngx") {
        error = "the recording's parameters are " + rec.parameter_source + "'s; DLSS-G's interface replays NGX's";
        return 1;
    }
    const record::Texture* backbuffer = rec.Find("backbuffer");
    const record::Texture* depth = rec.Find("depth");
    const record::Texture* motion = rec.Find("motion");
    if (!backbuffer || !depth || !motion) {
        error = "the recording lacks a backbuffer, depth or motion texture";
        return 1;
    }
    const DXGI_FORMAT out_format = DxgiOf(rec, *backbuffer);
    replay::ColorFormat color;
    if (!replay::ColorFormatOf((uint32_t)out_format, color)) {
        error = "the backbuffer's format " + std::to_string((int)out_format) + " is not one this tool writes";
        return 1;
    }
    const uint32_t width = rec.meta.value("width", backbuffer->width), height = rec.meta.value("height",
                                                                                               backbuffer->height);
    const uint32_t recorded_multiplier = rec.meta.value("multiplier", 2u);
    const uint32_t multiplier = o.multiplier ? o.multiplier : recorded_multiplier;
    if (multiplier < 2 || multiplier > 8) {
        error = "--multiplier is 2 to 8";
        return 1;
    }
    const std::string mode = !o.mode.empty() ? o.mode : rec.meta.value("mode", std::string("interleaved"));
    if (mode != "interleaved" && mode != "generated") {
        error = "--mode is interleaved or generated";
        return 1;
    }
    const size_t first = (size_t)std::clamp<int64_t>(o.first, 0, (int64_t)rec.index.size());
    const size_t last = o.last < 0 ? rec.index.size() : (size_t)std::clamp<int64_t>(o.last, (int64_t)first,
                                                                                      (int64_t)rec.index.size());
    if (first >= last) {
        error = "--frames selects no recorded frame (the recording has " + std::to_string(rec.index.size()) + ")";
        return 1;
    }
    std::unordered_map<uint64_t, std::vector<const record::Call*>> calls;
    for (const record::Call& c : rec.calls)
        calls[c.frame].push_back(&c);

    // The stream opens first: its standard output moves away before any DLL loads.
    StreamWriter stream;
    if (!o.stream.empty() && !stream.Open(o.stream, error))
        return 1;

    // The device, its queue and one list.
    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        char text[64];
        std::snprintf(text, sizeof(text), "D3D12CreateDevice: 0x%08x", (unsigned)hr);
        error = text;
        return 1;
    }
    std::string gpu;
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            gpu = name;
        }
    }
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    uint64_t fence_value = 0;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    auto submit = [&]() -> bool {
        if (FAILED(list->Close()))
            return false;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fence_value);
        fence->SetEventOnCompletion(fence_value, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        return device->GetDeviceRemovedReason() == S_OK;
    };
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &b);
    };
    auto texture = [&](uint32_t w, uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                       D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, format, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    };
    auto buffer = [&](uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES heap = {type};
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    };

    // The inputs: one texture per role, in its recorded format, filled through one upload buffer.
    struct Input {
        const record::Texture* t = nullptr;
        ComPtr<ID3D12Resource> resource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        bool swap = false;  // BGRA, decoded as RGBA
    };
    std::vector<Input> inputs;
    uint64_t upload_bytes = 0;
    nlohmann::json ignored = nlohmann::json::array();
    for (const record::Texture& t : rec.textures) {
        static const char* const ROLES[] = {"backbuffer", "hudless", "ui", "ui_alpha", "depth", "motion"};
        if (std::find_if(std::begin(ROLES), std::end(ROLES), [&](const char* r) { return t.name == r; }) ==
            std::end(ROLES)) {
            ignored.push_back(t.name);
            continue;
        }
        Input in;
        in.t = &t;
        const DXGI_FORMAT format = DxgiOf(rec, t);
        in.resource = texture(t.width, t.height, format,
                              DepthFamily(format) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST);
        if (!in.resource) {
            error = t.name + ": cannot create a texture of DXGI format " + std::to_string((int)format);
            return 1;
        }
        const D3D12_RESOURCE_DESC desc = in.resource->GetDesc();
        UINT rows = 0;
        UINT64 row_bytes = 0, total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, upload_bytes, &in.footprint, &rows, &row_bytes, &total);
        if (row_bytes != (UINT64)t.width * t.texel) {
            error = t.name + ": DXGI format " + std::to_string((int)format) + " has " +
                    std::to_string(row_bytes / std::max(t.width, 1u)) + "-byte texels, the recording " +
                    std::to_string(t.texel);
            return 1;
        }
        in.swap = t.vk == record::vk::B8G8R8A8_UNORM || t.vk == record::vk::B8G8R8A8_SRGB;
        upload_bytes = (upload_bytes + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                       ~(uint64_t)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        inputs.push_back(std::move(in));
    }
    ComPtr<ID3D12Resource> upload = buffer(upload_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* upload_data = nullptr;
    if (!upload || FAILED(upload->Map(0, nullptr, (void**)&upload_data))) {
        error = "cannot map an upload buffer of " + std::to_string(upload_bytes) + " bytes";
        return 1;
    }
    ID3D12Resource* backbuffer_resource = nullptr;
    replay::FgInputs fg_inputs;
    for (const Input& in : inputs) {
        fg_inputs.textures[in.t->name] = in.resource.Get();
        if (in.t->name == "backbuffer")
            backbuffer_resource = in.resource.Get();
    }

    // The outputs, one per generated frame and the real frame's copy, and their readback.
    const uint32_t count = multiplier - 1;
    std::vector<ComPtr<ID3D12Resource>> outputs;
    for (uint32_t i = 0; i <= count; i++) {
        ComPtr<ID3D12Resource> r = texture(width, height, out_format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!r && (out_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || out_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
            r = texture(width, height, out_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                                                     : DXGI_FORMAT_B8G8R8A8_UNORM,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!r) {
            error = "cannot create an output texture of DXGI format " + std::to_string((int)out_format);
            return 1;
        }
        outputs.push_back(r);
    }
    replay::FgOutputs fg_outputs;
    for (uint32_t i = 0; i < count; i++)
        fg_outputs.generated.push_back(outputs[i].Get());
    fg_outputs.real = outputs[count].Get();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> readback_at(count);
    uint64_t readback_bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        const D3D12_RESOURCE_DESC desc = outputs[i]->GetDesc();
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, readback_bytes, &readback_at[i], nullptr, nullptr, &total);
        readback_bytes = (readback_bytes + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                         ~(uint64_t)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    ComPtr<ID3D12Resource> readback = buffer(readback_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC query_desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
    ComPtr<ID3D12QueryHeap> queries;
    device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries));
    ComPtr<ID3D12Resource> stamps = buffer(16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 tick_frequency = 1;
    queue->GetTimestampFrequency(&tick_frequency);
    if (!readback || !queries || !stamps) {
        error = "cannot create the readback buffers";
        return 1;
    }

    // The generator.
    std::unique_ptr<replay::FgGenerator> generator = replay::MakeNgxGenerator(o.generator, o.dll, o.label, o.ngx_log,
                                                                              error);
    if (!generator)
        return 1;
    replay::FgSetup setup;
    setup.device = device.Get();
    setup.queue = queue.Get();
    setup.recording = &rec;
    setup.width = width;
    setup.height = height;
    setup.backbuffer_format = out_format;
    setup.render_width = depth->width;
    setup.render_height = depth->height;
    setup.multiplier = multiplier;
    if (!generator->Create(setup, list.Get(), error))
        return 1;
    if (!submit()) {
        error = "the device was lost creating the generator";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(o.out, ec);
    const double fps = FrameRate(rec, first, last, mode == "interleaved" ? multiplier : multiplier - 1);
    report = {{"format", "nr-replay-1"},
              {"recording", std::filesystem::path(o.record).lexically_normal().filename().string()},
              {"recording_path", o.record},
              {"game", rec.meta.value("game", "")},
              {"source", rec.source},
              {"generator", generator->Describe()},
              {"gpu", gpu},
              {"size", {width, height}},
              {"dxgi_format", (uint32_t)out_format},
              {"encoding", rec.meta.value("encoding", "")},
              {"hdr", rec.meta.value("hdr", false)},
              {"multiplier", multiplier},
              {"recorded_multiplier", recorded_multiplier},
              {"mode", mode},
              {"fps", fps},
              {"real_frames", {first, last}},
              {"raw", o.raw},
              {"stream", o.stream.empty() ? nlohmann::json() : nlohmann::json(o.stream)}};
    if (!ignored.empty())
        report["ignored_textures"] = ignored;
    if (report["recording"].get<std::string>().empty())
        report["recording"] = o.record;

    if (!o.stream.empty()) {
        const std::vector<uint8_t> header = replay::StreamHeaderBytes(report);
        if (!stream.Write(header.data(), header.size())) {
            error = "the stream's reader closed";
            return 1;
        }
    }

    // Three stages: a decoder thread reads and decodes real frames ahead, this thread uploads them, runs the
    // generator and reads its frames back, and a writer thread writes raw frames and the stream.
    Channel<std::unique_ptr<Decoded>> decoded(2);
    Channel<std::unique_ptr<OutFrame>> written(2 * multiplier);
    std::atomic<bool> stop{false};
    record::Pool pool(std::max(2u, std::thread::hardware_concurrency()));
    std::thread decoder([&] {
        record::DecodeScratch scratch;
        for (size_t k = first; k < last && !stop; k++) {
            auto d = std::make_unique<Decoded>();
            d->k = k;
            if (!record::Decode(rec, rec.index[k], pool, scratch, d->textures, d->error) && d->error.empty())
                d->error = "decode failed";
            const bool bad = !d->error.empty();
            if (!decoded.Push(std::move(d)) || bad)
                break;
        }
        decoded.Close();
    });
    std::string write_error;
    std::thread writer([&] {
        std::unique_ptr<OutFrame> f;
        replay::Image image;
        image.width = width;
        image.height = height;
        image.format = color;
        while (written.Pop(f)) {
            if (!write_error.empty())
                continue;
            if (o.raw) {
                image.texels = std::move(f->texels);
                if (!replay::WriteRaw(o.out + "/" + f->stem + ".nrraw", image, write_error))
                    stop = true;
                f->texels = std::move(image.texels);
            }
            if (!o.stream.empty() && write_error.empty() &&
                (!stream.Write(&f->header, sizeof(f->header)) || !stream.Write(f->texels.data(), f->texels.size()))) {
                write_error = "the stream's reader closed";
                stop = true;
            }
        }
    });
    struct Joiner {
        std::function<void()> run;
        ~Joiner() { run(); }
    } joiner{[&] {
        stop = true;
        decoded.Close();
        written.Close();
        if (decoder.joinable())
            decoder.join();
        if (writer.joinable())
            writer.join();
    }};

    nlohmann::json frames = nlohmann::json::array(), gaps = nlohmann::json::array(),
                   skipped = nlohmann::json::array();
    std::vector<double> gpu_ms;
    auto emit = [&](const char* kind, uint32_t a, uint32_t b, uint32_t index, std::vector<uint8_t> texels) {
        auto f = std::make_unique<OutFrame>();
        const double t = index ? (double)index / multiplier : 0.0;
        f->header.kind = index ? 1 : 0;
        f->header.a = a;
        f->header.b = b;
        f->header.index = index;
        f->header.t = (float)t;
        f->header.width = width;
        f->header.height = height;
        f->header.dxgi = (uint32_t)out_format;
        f->header.texel = color.texel;
        f->header.bytes = texels.size();
        f->stem = replay::FrameStem(kind, a, b, t);
        f->texels = std::move(texels);
        frames.push_back({{"kind", kind}, {"real", b}, {"a", a}, {"b", b}, {"index", index}, {"t", t},
                          {"file", o.raw ? nlohmann::json(f->stem + ".nrraw") : nlohmann::json()}});
        return written.Push(std::move(f));
    };
    const size_t row = (size_t)width * color.texel;
    int64_t previous = -1;  // the last replayed real frame, -1 at a run's start
    const auto started = std::chrono::steady_clock::now();
    size_t replayed = 0;
    for (size_t k = first; k < last; k++) {
        std::unique_ptr<Decoded> d;
        if (!decoded.Pop(d) || !d->error.empty() || d->k != k) {
            error = d ? "frame " + std::to_string(k) + ": " + d->error : "the decoder stopped";
            return 1;
        }
        const record::IndexRecord& ix = rec.index[k];
        auto it = calls.find(ix.frame);
        if (it == calls.end() || it->second.empty()) {
            skipped.push_back({{"frame", k}, {"why", "no calls recorded for it"}});
            previous = -1;
            continue;
        }
        replay::FgFrame frame;
        frame.calls = it->second;
        for (const record::Call* c : frame.calls) {
            record::FgCall typed;
            std::string why;
            record::Fg(rec, *c, typed, why);
            frame.typed.push_back(typed);
        }
        frame.count = count;
        frame.reset = previous < 0 || ix.dropped > 0;
        if (previous >= 0 && ix.dropped > 0)
            gaps.push_back({{"frame", k}, {"dropped", ix.dropped}});

        // Upload, generate, read back.
        for (const Input& in : inputs) {
            const std::vector<uint8_t>& texels = d->textures[in.t->name];
            const size_t bytes = (size_t)in.t->width * in.t->texel;
            if (texels.size() != bytes * in.t->height) {
                error = "frame " + std::to_string(k) + ": " + in.t->name + " decoded to the wrong size";
                return 1;
            }
            pool.For(in.t->height, [&](size_t y) {
                uint8_t* to = upload_data + in.footprint.Offset + y * in.footprint.Footprint.RowPitch;
                std::memcpy(to, texels.data() + y * bytes, bytes);
                if (in.swap)
                    for (size_t x = 0; x < bytes; x += 4)
                        std::swap(to[x], to[x + 2]);
            });
            D3D12_TEXTURE_COPY_LOCATION dst = {in.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint = in.footprint;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        barrier(backbuffer_resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        if (!generator->Evaluate(list.Get(), frame, fg_inputs, fg_outputs, error)) {
            error = "frame " + std::to_string(k) + ": " + error;
            return 1;
        }
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps.Get(), 0);
        if (!frame.reset)
            for (uint32_t i = 0; i < count; i++) {
                barrier(outputs[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint = readback_at[i];
                D3D12_TEXTURE_COPY_LOCATION src = {outputs[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                src.SubresourceIndex = 0;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(outputs[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        barrier(backbuffer_resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!submit()) {
            error = "frame " + std::to_string(k) + ": the device was lost";
            return 1;
        }
        if (!frame.reset) {
            UINT64* t = nullptr;
            if (SUCCEEDED(stamps->Map(0, nullptr, (void**)&t))) {
                gpu_ms.push_back((double)(t[1] - t[0]) * 1000.0 / (double)tick_frequency);
                stamps->Unmap(0, nullptr);
            }
            const uint8_t* mapped = nullptr;
            if (FAILED(readback->Map(0, nullptr, (void**)&mapped))) {
                error = "cannot map the readback buffer";
                return 1;
            }
            for (uint32_t i = 0; i < count; i++) {
                std::vector<uint8_t> texels(row * height);
                const uint8_t* from = mapped + readback_at[i].Offset;
                pool.For(height, [&](size_t y) {
                    uint8_t* to = texels.data() + y * row;
                    std::memcpy(to, from + y * readback_at[i].Footprint.RowPitch, row);
                    if (color.bgra)
                        for (size_t x = 0; x < row; x += 4)
                            std::swap(to[x], to[x + 2]);
                });
                if (!emit("generated", (uint32_t)previous, (uint32_t)k, i + 1, std::move(texels)))
                    break;
            }
            readback->Unmap(0, nullptr);
        }
        if (mode == "interleaved")
            emit("real", (uint32_t)k, (uint32_t)k, 0, std::move(d->textures["backbuffer"]));
        if (stop)
            break;
        previous = (int64_t)k;
        if (++replayed % 100 == 0) {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::fprintf(stderr, "nr-replay: %zu of %zu real frames, %.1f per second\n", replayed, last - first,
                         replayed / s);
        }
    }
    joiner.run();
    joiner.run = [] {};
    if (!o.stream.empty() && write_error.empty()) {
        replay::StreamFrame end;
        std::memcpy(end.magic, "NRSE", 4);
        stream.Write(&end, sizeof(end));
    }
    generator->Release();
    std::sort(gpu_ms.begin(), gpu_ms.end());
    report["frames"] = frames;
    report["gaps"] = gaps;
    report["skipped"] = skipped;
    report["gpu_ms_median"] = gpu_ms.empty() ? nlohmann::json() : nlohmann::json(gpu_ms[gpu_ms.size() / 2]);
    report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!write_error.empty()) {
        error = write_error;
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::string error;
    if (!Parse(argc, argv, o, error)) {
        std::fprintf(stderr, "nr-replay: %s\n%s", error.c_str(), USAGE);
        return 2;
    }
    nlohmann::json report;
    const int code = Run(o, report, error);
    if (!report.is_object())
        report = nlohmann::json::object();
    report["status"] = code ? "failed: " + error : "complete";
    std::error_code ec;
    std::filesystem::create_directories(o.out, ec);
    std::ofstream(o.out + "/replay.json") << report.dump(1) << "\n";
    if (code)
        std::fprintf(stderr, "nr-replay: %s\n", error.c_str());
    // A summary line; on standard error when the stream uses standard output.
    nlohmann::json summary = {{"status", report["status"]}, {"out", o.out}};
    for (const char* k : {"generator", "size", "multiplier", "mode", "fps", "gpu_ms_median", "seconds"})
        if (report.contains(k))
            summary[k] = report[k];
    if (report.contains("frames"))
        summary["frames"] = report["frames"].size();
    std::fprintf(o.stream == "-" ? stderr : stdout, "%s\n", summary.dump().c_str());
    return code;
}
