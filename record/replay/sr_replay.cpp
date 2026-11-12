// nr-replay for SR and RR recordings (README.md "SR and RR"): each recorded frame's inputs are decoded, uploaded to
// D3D12 textures of their recorded formats, and given to the generator with the call's recorded parameters; its output
// is read back and written as raw frames and/or a frame stream, one output frame per recorded frame.
//
//   nr-replay --record DIR --out DIR --generator dlss|dlssd|ngx|fsr|xess|recorded [--dll PATH] [--label TEXT]
//       [--frames A:B] [--preset X] [--raw] [--stream PATH|-] [--ngx-log]

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "record/reader.h"
#include "record/replay/frames.h"
#include "record/replay/replay_io.h"
#include "record/replay/sr_generator.h"

using Microsoft::WRL::ComPtr;

namespace replay {

namespace {

constexpr const char* USAGE =
    "usage: nr-replay --record DIR --out DIR --generator dlss|dlssd|ngx|fsr|xess|recorded [--dll PATH]\n"
    "                 [--label TEXT] [--frames A:B] [--preset X] [--raw] [--stream PATH|-] [--ngx-log]\n";

struct Options {
    std::string record, out, generator, dll, label, stream;
    int64_t first = 0, last = -1;
    int preset = -1;
    bool raw = false, ngx_log = false;
};

bool Parse(int argc, char** argv, Options& o, std::string& error) {
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        if (k == "--raw" || k == "--ngx-log") {
            (k == "--raw" ? o.raw : o.ngx_log) = true;
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
        else if (k == "--stream") o.stream = v;
        else if (k == "--preset") {
            if (v.size() != 1 || std::toupper((unsigned char)v[0]) < 'A' || std::toupper((unsigned char)v[0]) > 'Z') {
                error = "--preset is a render preset letter (A to Z), as NGX names them";
                return false;
            }
            o.preset = std::toupper((unsigned char)v[0]) - 'A' + 1;
        } else if (k == "--frames") {
            const size_t colon = v.find(':');
            if (colon == std::string::npos) {
                error = "--frames is A:B (recorded frames A to B - 1; A: and :B leave an end open)";
                return false;
            }
            o.first = colon ? std::stoll(v.substr(0, colon)) : 0;
            o.last = colon + 1 < v.size() ? std::stoll(v.substr(colon + 1)) : -1;
        } else {
            error = "unknown option " + k;
            return false;
        }
    }
    if (o.record.empty() || o.out.empty() || o.generator.empty() || (o.dll.empty() && o.generator != "recorded")) {
        error = "--record, --out, --generator and (but for recorded) --dll are needed";
        return false;
    }
    if (!o.raw && o.stream.empty()) {
        error = "nothing to write: give --raw (raw frames in --out) and/or --stream PATH|-";
        return false;
    }
    return true;
}

// A texture format the generators can read: typeless formats as their typed form, depth planes as depth formats.
DXGI_FORMAT Typed(DXGI_FORMAT f, bool depth) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return depth ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default: return f;
    }
}

// The recorded output's texels as FP16 RGBA (the replay's output layout).
bool ToHalfRgba(const std::vector<uint8_t>& in, DXGI_FORMAT format, size_t texels, std::vector<uint8_t>& out) {
    out.resize(texels * 8);
    uint16_t* o = (uint16_t*)out.data();
    auto put = [&](size_t i, float r, float g, float b, float a) {
        o[4 * i] = ToHalf(r), o[4 * i + 1] = ToHalf(g), o[4 * i + 2] = ToHalf(b), o[4 * i + 3] = ToHalf(a);
    };
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        out = in;
        return out.size() == texels * 8;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        for (size_t i = 0; i < texels; i++) {
            uint32_t w;
            std::memcpy(&w, in.data() + 4 * i, 4);
            // each channel's bits are the top bits of an FP16 value
            o[4 * i] = (uint16_t)((w & 0x7FF) << 4);
            o[4 * i + 1] = (uint16_t)(((w >> 11) & 0x7FF) << 4);
            o[4 * i + 2] = (uint16_t)(((w >> 22) & 0x3FF) << 5);
            o[4 * i + 3] = ToHalf(1);
        }
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        for (size_t i = 0; i < texels; i++) {
            uint32_t w;
            std::memcpy(&w, in.data() + 4 * i, 4);
            put(i, (w & 0x3FF) / 1023.0f, ((w >> 10) & 0x3FF) / 1023.0f, ((w >> 20) & 0x3FF) / 1023.0f,
                (w >> 30) / 3.0f);
        }
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:  // decoded as RGBA
        for (size_t i = 0; i < texels; i++)
            put(i, in[4 * i] / 255.0f, in[4 * i + 1] / 255.0f, in[4 * i + 2] / 255.0f, in[4 * i + 3] / 255.0f);
        return true;
    default:
        return false;
    }
}

struct Decoded {
    size_t k = 0;
    std::map<std::string, std::vector<uint8_t>> textures;
    std::string error;
};

struct OutFrame {
    StreamFrame header;
    std::string stem;
    std::vector<uint8_t> texels;
};

int Run(const Options& o, nlohmann::json& report, std::string& error) {
    record::Recording rec;
    if (!record::Open(o.record, rec, error))
        return 1;
    if (rec.parameter_source != "ngx") {
        error = "the recording's parameters are " + rec.parameter_source + "'s; the SR generators take NGX's";
        return 1;
    }
    const record::Texture* color = rec.Find("color");
    const record::Texture* recorded_output = rec.Find("output");
    if (!color || !rec.Find("depth") || !rec.Find("motion")) {
        error = "the recording lacks a color, depth or motion texture";
        return 1;
    }
    const bool from_recording = o.generator == "recorded";
    if (from_recording && !recorded_output) {
        error = "--generator recorded: the recording has no output texture (record_output was off)";
        return 1;
    }
    const size_t first = (size_t)std::clamp<int64_t>(o.first, 0, (int64_t)rec.index.size());
    const size_t last = o.last < 0 ? rec.index.size() : (size_t)std::clamp<int64_t>(o.last, (int64_t)first,
                                                                                      (int64_t)rec.index.size());
    if (first >= last) {
        error = "--frames selects no recorded frame (the recording has " + std::to_string(rec.index.size()) + ")";
        return 1;
    }
    std::unordered_map<uint64_t, const record::Call*> calls;
    for (const record::Call& c : rec.calls)
        calls.emplace(c.frame, &c);
    record::SrCall first_call;
    for (size_t k = first; k < last; k++)
        if (auto it = calls.find(rec.index[k].frame); it != calls.end()) {
            record::Sr(rec, *it->second, first_call, error);
            break;
        }
    const uint32_t width = rec.meta.value("width", first_call.output_size[0]);
    const uint32_t height = rec.meta.value("height", first_call.output_size[1]);
    if (!width || !height) {
        error = "the recording gives no output size";
        return 1;
    }
    const DXGI_FORMAT out_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ColorFormat out_color;
    ColorFormatOf((uint32_t)out_format, out_color);

    StreamWriter stream;
    if (!o.stream.empty() && !stream.Open(o.stream, error))
        return 1;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fence_value = 0;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    std::string gpu;
    std::unique_ptr<SrGenerator> generator;
    nlohmann::json described;
    if (from_recording) {
        described = {{"kind", "recorded"},
                     {"label", o.label.empty() ? "recorded (" + rec.meta.value("forward", std::string("game")) + ")"
                                               : o.label},
                     {"forward", rec.meta.value("forward", nlohmann::json())}};
    } else {
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        if (FAILED(hr)) {
            char text[64];
            std::snprintf(text, sizeof(text), "D3D12CreateDevice: 0x%08x", (unsigned)hr);
            error = text;
            return 1;
        }
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
        D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT};
        device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        generator = MakeSrGenerator(o.generator, o.dll, o.label, o.ngx_log, error);
        if (!generator)
            return 1;
    }
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
    auto buffer = [&](uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES heap = {type};
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    };

    // The inputs (every recorded texture but the output), the output and the readback.
    struct Input {
        const record::Texture* t = nullptr;
        ComPtr<ID3D12Resource> resource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        bool swap = false;  // BGRA, decoded as RGBA
    };
    std::vector<Input> inputs;
    SrInputs sr_inputs;
    ComPtr<ID3D12Resource> upload, output, readback, stamps;
    ComPtr<ID3D12QueryHeap> queries;
    uint8_t* upload_data = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_at = {};
    UINT64 tick_frequency = 1;
    if (!from_recording) {
        uint64_t upload_bytes = 0;
        for (const record::Texture& t : rec.textures) {
            if (t.name == "output")
                continue;
            Input in;
            in.t = &t;
            const DXGI_FORMAT format = Typed(DxgiOf(rec, t), t.depth);
            D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, t.width, t.height, 1, 1, format, {1, 0},
                                        D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                        DepthFamily(format) || format == DXGI_FORMAT_D16_UNORM
                                            ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE};
            D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&in.resource)))) {
                error = t.name + ": cannot create a texture of DXGI format " + std::to_string((int)format);
                return 1;
            }
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
            sr_inputs.textures[t.name] = in.resource.Get();
            inputs.push_back(std::move(in));
        }
        upload = buffer(upload_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!upload || FAILED(upload->Map(0, nullptr, (void**)&upload_data))) {
            error = "cannot map an upload buffer of " + std::to_string(upload_bytes) + " bytes";
            return 1;
        }
        D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, out_format, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        nullptr, IID_PPV_ARGS(&output));
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &readback_at, nullptr, nullptr, &total);
        readback = buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC query_desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
        device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries));
        stamps = buffer(16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        queue->GetTimestampFrequency(&tick_frequency);
        if (!output || !readback || !queries || !stamps) {
            error = "cannot create the output and readback resources";
            return 1;
        }
        SrSetup setup;
        setup.device = device.Get();
        setup.queue = queue.Get();
        setup.recording = &rec;
        setup.render_width = color->width;
        setup.render_height = color->height;
        setup.width = width;
        setup.height = height;
        setup.output_format = out_format;
        setup.first = first_call;
        setup.preset = o.preset;
        if (!generator->Create(setup, list.Get(), error))
            return 1;
        if (!submit()) {
            error = "the device was lost creating the generator";
            return 1;
        }
        described = generator->Describe();
    }

    std::error_code ec;
    std::filesystem::create_directories(o.out, ec);
    const bool hdr = rec.meta.value("hdr", first_call.hdr);
    report = {{"format", "nr-replay-1"},
              {"feature", rec.feature},
              {"recording", std::filesystem::path(o.record).lexically_normal().filename().string()},
              {"recording_path", o.record},
              {"game", rec.meta.value("game", "")},
              {"source", rec.source},
              {"generator", described},
              {"gpu", gpu},
              {"size", {width, height}},
              {"render_size", {color->width, color->height}},
              {"dxgi_format", (uint32_t)out_format},
              {"encoding", hdr ? "linear HDR" : "linear SDR"},
              {"hdr", hdr},
              {"multiplier", 1},
              {"mode", "output"},
              {"fps", FrameRate(rec, first, last, 1)},
              {"real_frames", {first, last}},
              {"raw", o.raw},
              {"stream", o.stream.empty() ? nlohmann::json() : nlohmann::json(o.stream)}};
    if (report["recording"].get<std::string>().empty())
        report["recording"] = o.record;
    if (!o.stream.empty()) {
        const std::vector<uint8_t> header = StreamHeaderBytes(report);
        if (!stream.Write(header.data(), header.size())) {
            error = "the stream's reader closed";
            return 1;
        }
    }

    // A decoder thread reads and decodes frames ahead, this thread runs the generator, a writer thread writes.
    Channel<std::unique_ptr<Decoded>> decoded(2);
    Channel<std::unique_ptr<OutFrame>> written(2);
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
        Image image;
        image.width = width;
        image.height = height;
        image.format = out_color;
        while (written.Pop(f)) {
            if (!write_error.empty())
                continue;
            if (o.raw) {
                image.texels = std::move(f->texels);
                if (!WriteRaw(o.out + "/" + f->stem + ".nrraw", image, write_error))
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
    auto emit = [&](uint32_t k, std::vector<uint8_t> texels) {
        auto f = std::make_unique<OutFrame>();
        f->header.kind = 2;
        f->header.a = f->header.b = k;
        f->header.width = width;
        f->header.height = height;
        f->header.dxgi = (uint32_t)out_format;
        f->header.texel = out_color.texel;
        f->header.bytes = texels.size();
        f->stem = FrameStem("output", k, k, 0);
        f->texels = std::move(texels);
        frames.push_back({{"kind", "output"}, {"real", k}, {"a", k}, {"b", k}, {"index", 0}, {"t", 0.0},
                          {"file", o.raw ? nlohmann::json(f->stem + ".nrraw") : nlohmann::json()}});
        return written.Push(std::move(f));
    };
    const size_t row = (size_t)width * out_color.texel;
    int64_t previous = -1;
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
        if (it == calls.end()) {
            skipped.push_back({{"frame", k}, {"why", "no call recorded for it"}});
            previous = -1;
            continue;
        }
        if (previous >= 0 && ix.dropped > 0)
            gaps.push_back({{"frame", k}, {"dropped", ix.dropped}});
        if (from_recording) {
            std::vector<uint8_t> texels;
            if (!ToHalfRgba(d->textures["output"], DxgiOf(rec, *recorded_output),
                            (size_t)recorded_output->width * recorded_output->height, texels) ||
                recorded_output->width != width || recorded_output->height != height) {
                error = "the recorded output's format " + std::to_string(recorded_output->api_format) +
                        " or size is not one this tool converts";
                return 1;
            }
            emit((uint32_t)k, std::move(texels));
        } else {
            SrFrame frame;
            frame.call = it->second;
            std::string why;
            record::Sr(rec, *frame.call, frame.typed, why);
            frame.reset = previous < 0 || ix.dropped > 0;
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
                barrier(in.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            if (!generator->Evaluate(list.Get(), frame, sr_inputs, output.Get(), error)) {
                error = "frame " + std::to_string(k) + ": " + error;
                return 1;
            }
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps.Get(), 0);
            barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = readback_at;
            D3D12_TEXTURE_COPY_LOCATION src = {output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (const Input& in : inputs)
                barrier(in.resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
            if (!submit()) {
                error = "frame " + std::to_string(k) + ": the device was lost";
                return 1;
            }
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
            std::vector<uint8_t> texels(row * height);
            pool.For(height, [&](size_t y) {
                std::memcpy(texels.data() + y * row, mapped + readback_at.Offset + y * readback_at.Footprint.RowPitch,
                            row);
            });
            readback->Unmap(0, nullptr);
            emit((uint32_t)k, std::move(texels));
        }
        if (stop)
            break;
        previous = (int64_t)k;
        if (++replayed % 100 == 0) {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::fprintf(stderr, "nr-replay: %zu of %zu frames, %.1f per second\n", replayed, last - first,
                         replayed / s);
        }
    }
    joiner.run();
    joiner.run = [] {};
    if (!o.stream.empty() && write_error.empty()) {
        StreamFrame end;
        std::memcpy(end.magic, "NRSE", 4);
        stream.Write(&end, sizeof(end));
    }
    if (generator)
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

bool IsSrRecording(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--record") {
            std::ifstream in(std::string(argv[i + 1]) + "/record.json");
            const nlohmann::json meta = nlohmann::json::parse(in, nullptr, false);
            const std::string feature = meta.is_object() ? meta.value("feature", "fg") : "fg";
            return feature == "sr" || feature == "rr";
        }
    return false;
}

int RunSr(int argc, char** argv) {
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
    nlohmann::json summary = {{"status", report["status"]}, {"out", o.out}};
    for (const char* k : {"feature", "generator", "size", "render_size", "fps", "gpu_ms_median", "seconds"})
        if (report.contains(k))
            summary[k] = report[k];
    if (report.contains("frames"))
        summary["frames"] = report["frames"].size();
    std::fprintf(o.stream == "-" ? stderr : stdout, "%s\n", summary.dump().c_str());
    return code;
}

}  // namespace replay
