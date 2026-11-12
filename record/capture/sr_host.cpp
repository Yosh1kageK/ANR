// nr-sr-host: a synthetic game for checking the SR capture DLL (README.md "Checking it"). It renders a moving scene on
// the CPU at render size with a Halton jitter, as a game that uses DLSS gives it to NGX: scene-linear HDR color,
// inverted depth, motion in render pixels (current to previous, without jitter) and for RR the guide buffers; it calls
// an NGX DLL's D3D12 exports per frame (the SR capture DLL itself, or OptiScaler as nvngx or dxgi.dll with the capture
// DLL as its custom upscaler) and presses the record key on chosen frames. It prints one JSON line.
//
//   nr-sr-host --ngx DLL [--feature sr|rr] [--frames N] [--render WxH] [--output WxH] [--color rgba16f|r11g11b10]
//       [--record-frames A,B] [--quality Q] [--path DIR] [--dump-color F PATH]
//
// --record-frames A,B holds the record key (F6) down during frames A and B: a recording of frames A to B - 1.
// --path is a folder for NGX's search path (NVIDIA's nvngx_dlss.dll and nvngx_dlssd.dll). --dump-color writes frame F's
// color texels as uploaded, for checking that a recording holds them bit for bit.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_params.h>

#include "record/replay/ngx_common.h"

using Microsoft::WRL::ComPtr;

namespace {

uint16_t Half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int exponent = (int)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFF;
    if (exponent <= 0)
        return (uint16_t)sign;
    if (exponent >= 31)
        return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | exponent << 10 | ((mantissa + 0x1000) >> 13));
}

// An unsigned float of 6 (11-bit) or 5 (10-bit) mantissa bits: the top bits of the FP16 value.
uint32_t Small(float f, int mantissa) {
    const uint16_t h = Half(std::max(f, 0.0f));
    return mantissa == 6 ? (h >> 4) & 0x7FF : (h >> 5) & 0x3FF;
}

float Halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    for (; i; i /= base) {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}

// The scene at time t, seen at render pixel (x, y) of a w x h frame: a scrolling checker and ring pattern behind a
// spinning disc. Color is scene-linear HDR; depth inverted (near 1); motion to where the point was at t - 1.
struct Sample {
    float r, g, b, depth, mx, my, albedo, normal_z, roughness;
};

Sample Scene(float x, float y, float t, uint32_t w, uint32_t h) {
    const float scale = (float)h / 540.0f, pan = 3.0f * scale;  // render pixels per frame
    const float cx = 0.5f * w, cy = 0.5f * h, radius = 0.28f * h, spin = 0.03f;
    const float dx = x - cx, dy = y - cy;
    Sample s = {};
    if (dx * dx + dy * dy < radius * radius) {
        const float a = std::atan2(dy, dx) + spin * t, d = std::sqrt(dx * dx + dy * dy) / radius;
        const float stripes = 0.5f + 0.5f * std::cos(12.0f * a) * (0.6f + 0.4f * std::cos(30.0f * d));
        s = {2.5f * stripes + 0.1f, 0.6f * stripes + 0.05f, 0.15f, 0.9f, 0, 0, 0.7f, 1.0f, 0.3f};
        const float px = cx + dx * std::cos(spin) + dy * std::sin(spin), py = cy - dx * std::sin(spin) + dy * std::cos(spin);
        s.mx = px - x, s.my = py - y;
        return s;
    }
    const float u = (x + pan * t) / (24.0f * scale), v = y / (24.0f * scale);
    const bool checker = ((int)std::floor(u) + (int)std::floor(v)) & 1;
    const float rings = 0.5f + 0.5f * std::sin(0.08f * ((x + pan * t) * (x + pan * t) + y * y) / (scale * w));
    const float l = (checker ? 0.9f : 0.15f) * (0.7f + 0.3f * rings);
    s = {0.6f * l, 0.8f * l, 1.2f * l, 0.2f, -pan, 0, checker ? 0.8f : 0.2f, 1.0f, 0.6f};
    return s;
}

struct Options {
    std::string ngx, feature = "sr", color = "rgba16f", path, dump_path;
    int64_t dump_frame = -1;
    uint32_t frames = 60, rw = 960, rh = 540, ow = 1920, oh = 1080, quality = 1;
    std::vector<uint32_t> record_frames;
};

bool ParseSize(const std::string& v, uint32_t& w, uint32_t& h) {
    return std::sscanf(v.c_str(), "%ux%u", &w, &h) == 2 && w && h;
}

int Fail(const std::string& message) {
    std::printf("%s\n", nlohmann::json({{"status", "failed: " + message}}).dump().c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--ngx") o.ngx = v;
        else if (k == "--feature") o.feature = v;
        else if (k == "--frames") o.frames = (uint32_t)std::stoul(v);
        else if (k == "--render" && ParseSize(v, o.rw, o.rh)) {}
        else if (k == "--output" && ParseSize(v, o.ow, o.oh)) {}
        else if (k == "--color") o.color = v;
        else if (k == "--quality") o.quality = (uint32_t)std::stoul(v);
        else if (k == "--path") o.path = v;
        else if (k == "--dump-color" && i + 2 < argc) o.dump_frame = std::stoll(v), o.dump_path = argv[i + 2], i++;
        else if (k == "--record-frames") {
            for (size_t at = 0; at < v.size();) {
                const size_t comma = std::min(v.find(',', at), v.size());
                o.record_frames.push_back((uint32_t)std::stoul(v.substr(at, comma - at)));
                at = comma + 1;
            }
        } else return Fail("unknown option or value: " + k + " " + v);
    }
    if (o.ngx.empty())
        return Fail("--ngx DLL is needed");
    const bool rr = o.feature == "rr";

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        return Fail("D3D12CreateDevice failed");
    D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    uint64_t fence_value = 0;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    auto submit = [&] {
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fence_value);
        fence->SetEventOnCompletion(fence_value, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        return device->GetDeviceRemovedReason() == S_OK;
    };

    // The NGX DLL.
    HMODULE ngx = LoadLibraryA(o.ngx.c_str());
    if (!ngx)
        return Fail("cannot load " + o.ngx + " (error " + std::to_string(GetLastError()) + ")");
    auto get = [&](const char* name) { return (void*)GetProcAddress(ngx, name); };
    auto init = (replay::PFN_NgxInit)get("NVSDK_NGX_D3D12_Init_Ext");
    auto allocate = (replay::PFN_NgxParams)get("NVSDK_NGX_D3D12_AllocateParameters");
    auto create = (replay::PFN_NgxCreate)get("NVSDK_NGX_D3D12_CreateFeature");
    auto evaluate = (replay::PFN_NgxEvaluate)get("NVSDK_NGX_D3D12_EvaluateFeature");
    auto release = (replay::PFN_NgxRelease)get("NVSDK_NGX_D3D12_ReleaseFeature");
    auto shutdown = (replay::PFN_NgxShutdown1)get("NVSDK_NGX_D3D12_Shutdown1");
    if (!init || !create || !evaluate || !release)
        return Fail(o.ngx + " lacks NGX's D3D12 exports");
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string exe_dir = exe;
    exe_dir = exe_dir.substr(0, exe_dir.find_last_of("\\/"));
    const std::wstring folder = replay::Wide(o.path.empty() ? exe_dir : o.path);
    const wchar_t* paths[] = {folder.c_str()};
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    NVSDK_NGX_Result r = init(1, temp, device.Get(), NVSDK_NGX_Version_API, &common);
    if (NVSDK_NGX_FAILED(r))
        return Fail("Init_Ext: " + replay::Hex(r));
    replay::ParameterMap map;
    NVSDK_NGX_Parameter* p = &map;
    if (allocate && NVSDK_NGX_SUCCEED(allocate(&p)) && p) {
    } else {
        p = &map;
    }

    // The textures: inputs NON_PIXEL_SHADER_RESOURCE (as DLSS takes them), the output a UAV.
    auto texture = [&](uint32_t w, uint32_t h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES s) {
        D3D12_RESOURCE_DESC desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, f, {1, 0},
                                    D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
        ComPtr<ID3D12Resource> res;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, s, nullptr, IID_PPV_ARGS(&res));
        return res;
    };
    const DXGI_FORMAT color_format =
        o.color == "r11g11b10" ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    struct Input {
        const char* parameter;
        DXGI_FORMAT format;
        uint32_t texel;
        ComPtr<ID3D12Resource> resource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT at = {};
        std::vector<uint8_t> texels;
    };
    std::vector<Input> inputs = {{"Color", color_format, color_format == DXGI_FORMAT_R11G11B10_FLOAT ? 4u : 8u},
                                 {"Depth", DXGI_FORMAT_R32_FLOAT, 4},
                                 {"MotionVectors", DXGI_FORMAT_R16G16_FLOAT, 4}};
    if (rr) {
        inputs.push_back({"DLSS.Input.DiffuseAlbedo", DXGI_FORMAT_R8G8B8A8_UNORM, 4});
        inputs.push_back({"DLSS.Input.SpecularAlbedo", DXGI_FORMAT_R8G8B8A8_UNORM, 4});
        inputs.push_back({"GBuffer.Normals", DXGI_FORMAT_R16G16B16A16_FLOAT, 8});
        inputs.push_back({"GBuffer.Roughness", DXGI_FORMAT_R8_UNORM, 1});
    }
    uint64_t upload_bytes = 0;
    for (Input& in : inputs) {
        in.resource = texture(o.rw, o.rh, in.format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_RESOURCE_DESC desc = in.resource->GetDesc();
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, upload_bytes, &in.at, nullptr, nullptr, &total);
        upload_bytes = (upload_bytes + total + 511) & ~511ull;
        in.texels.resize((size_t)o.rw * o.rh * in.texel);
    }
    ComPtr<ID3D12Resource> output = texture(o.ow, o.oh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC buffer_desc = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, upload_bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                       {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    D3D12_HEAP_PROPERTIES upload_heap = {D3D12_HEAP_TYPE_UPLOAD};
    ComPtr<ID3D12Resource> upload;
    uint8_t* mapped = nullptr;
    device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    upload->Map(0, nullptr, (void**)&mapped);
    const D3D12_RESOURCE_DESC out_desc = output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_at = {};
    UINT64 readback_bytes = 0;
    device->GetCopyableFootprints(&out_desc, 0, 1, 0, &readback_at, nullptr, nullptr, &readback_bytes);
    buffer_desc.Width = readback_bytes;
    D3D12_HEAP_PROPERTIES readback_heap = {D3D12_HEAP_TYPE_READBACK};
    ComPtr<ID3D12Resource> readback;
    device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x = {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        list->ResourceBarrier(1, &x);
    };

    // The feature, as DLSS's helpers create it.
    const unsigned flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                           NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, o.rw);
    p->Set(NVSDK_NGX_Parameter_Height, o.rh);
    p->Set(NVSDK_NGX_Parameter_OutWidth, o.ow);
    p->Set(NVSDK_NGX_Parameter_OutHeight, o.oh);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, o.quality);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    if (rr) {
        p->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, (unsigned)NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
        p->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, (unsigned)NVSDK_NGX_DLSS_Roughness_Mode_Unpacked);
        p->Set(NVSDK_NGX_Parameter_Use_HW_Depth, (unsigned)NVSDK_NGX_DLSS_Depth_Type_HW);
    }
    NVSDK_NGX_Handle* handle = nullptr;
    r = create(list.Get(), rr ? NVSDK_NGX_Feature_RayReconstruction : NVSDK_NGX_Feature_SuperSampling, p, &handle);
    if (NVSDK_NGX_FAILED(r) || !submit())
        return Fail("CreateFeature: " + replay::Hex(r));

    const float near_plane = 0.1f, far_plane = 1000.0f, fov = 1.0f, aspect = (float)o.ow / (float)o.oh;
    const float y = 1.0f / std::tan(fov / 2);
    const float view_to_clip[16] = {y / aspect, 0, 0, 0, 0, y, 0, 0, 0, 0, near_plane / (near_plane - far_plane), 1,
                                    0, 0, -far_plane * near_plane / (near_plane - far_plane), 0};
    const float world_to_view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    uint32_t seed = 1;
    int failures = 0;
    std::string first_failure;
    double sum[3] = {};
    for (uint32_t frame = 0; frame < o.frames; frame++) {
        // Halton(2, 3) jitter, 8 phases per output-to-render pixel ratio as DLSS's guide asks; the sample's offset from
        // the pixel center is s, and NGX takes the offset that removes it, -s.
        const uint32_t phases = (uint32_t)std::ceil(8.0 * o.ow * o.oh / ((double)o.rw * o.rh));
        const float sx = Halton(frame % phases + 1, 2) - 0.5f, sy = Halton(frame % phases + 1, 3) - 0.5f;
        for (uint32_t yy = 0; yy < o.rh; yy++)
            for (uint32_t x = 0; x < o.rw; x++) {
                const size_t i = (size_t)yy * o.rw + x;
                Sample s = Scene(x + 0.5f + sx, yy + 0.5f + sy, (float)frame, o.rw, o.rh);
                const Sample c = Scene(x + 0.5f, yy + 0.5f, (float)frame, o.rw, o.rh);  // motion without jitter
                if (rr) {  // RR gets a noisy one-sample color
                    seed = seed * 1664525u + 1013904223u;
                    const float n = 0.4f + 1.2f * (float)(seed >> 8) / 16777216.0f;
                    s.r *= n, s.g *= n, s.b *= n;
                }
                if (color_format == DXGI_FORMAT_R11G11B10_FLOAT) {
                    const uint32_t w = Small(s.r, 6) | Small(s.g, 6) << 11 | Small(s.b, 5) << 22;
                    std::memcpy(&inputs[0].texels[4 * i], &w, 4);
                } else {
                    const uint16_t h[4] = {Half(s.r), Half(s.g), Half(s.b), Half(1)};
                    std::memcpy(&inputs[0].texels[8 * i], h, 8);
                }
                std::memcpy(&inputs[1].texels[4 * i], &s.depth, 4);
                const uint16_t mv[2] = {Half(c.mx), Half(c.my)};
                std::memcpy(&inputs[2].texels[4 * i], mv, 4);
                if (rr) {
                    const uint8_t a = (uint8_t)(255 * s.albedo);
                    const uint8_t da[4] = {a, a, a, 255}, sa[4] = {40, 40, 40, 255};
                    std::memcpy(&inputs[3].texels[4 * i], da, 4);
                    std::memcpy(&inputs[4].texels[4 * i], sa, 4);
                    const uint16_t nrm[4] = {Half(0), Half(0), Half(-1), Half(0)};
                    std::memcpy(&inputs[5].texels[8 * i], nrm, 8);
                    inputs[6].texels[i] = (uint8_t)(255 * s.roughness);
                }
            }
        if ((int64_t)frame == o.dump_frame) {
            if (std::FILE* f = std::fopen(o.dump_path.c_str(), "wb")) {
                std::fwrite(inputs[0].texels.data(), 1, inputs[0].texels.size(), f);
                std::fclose(f);
            }
        }
        for (Input& in : inputs) {
            const size_t row = (size_t)o.rw * in.texel;
            for (uint32_t yy = 0; yy < o.rh; yy++)
                std::memcpy(mapped + in.at.Offset + yy * in.at.Footprint.RowPitch, in.texels.data() + yy * row, row);
            D3D12_TEXTURE_COPY_LOCATION dst = {in.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            D3D12_TEXTURE_COPY_LOCATION src = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint = in.at;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(in.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            p->Set(in.parameter, in.resource.Get());
        }
        p->Set(NVSDK_NGX_Parameter_Output, output.Get());
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, -sx);
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, -sy);
        p->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
        p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
        p->Set(NVSDK_NGX_Parameter_Reset, frame == 0 ? 1 : 0);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, o.rw);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, o.rh);
        p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
        p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
        p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 1000.0f / 60);
        if (rr) {
            p->Set(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, (void*)world_to_view);
            p->Set(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, (void*)view_to_clip);
        }
        const bool key = std::find(o.record_frames.begin(), o.record_frames.end(), frame) != o.record_frames.end();
        if (key)
            keybd_event(VK_F6, 0, 0, 0);
        r = evaluate(list.Get(), handle, p, nullptr);
        if (NVSDK_NGX_FAILED(r) && !failures++)
            first_failure = "frame " + std::to_string(frame) + ": EvaluateFeature " + replay::Hex(r);
        for (Input& in : inputs)
            barrier(in.resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        const bool last = frame + 1 == o.frames;
        if (last) {
            barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = readback_at;
            D3D12_TEXTURE_COPY_LOCATION src = {output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (!submit())
            return Fail("device lost at frame " + std::to_string(frame));
        if (key) {
            Sleep(20);
            keybd_event(VK_F6, 0, KEYEVENTF_KEYUP, 0);
        }
        if (last) {
            const uint8_t* data = nullptr;
            readback->Map(0, nullptr, (void**)&data);
            for (uint32_t yy = 0; yy < o.oh; yy++)
                for (uint32_t x = 0; x < o.ow; x++)
                    for (int c = 0; c < 3; c++) {
                        uint16_t h;
                        std::memcpy(&h, data + readback_at.Offset + yy * readback_at.Footprint.RowPitch + 8 * x + 2 * c,
                                    2);
                        const uint32_t e = (h >> 10) & 31, m = h & 1023;
                        const float v = e == 0 ? m / 1024.0f / 16384.0f : std::ldexp(1.0f + m / 1024.0f, (int)e - 15);
                        sum[c] += e == 31 ? 0.0 : v;
                    }
            readback->Unmap(0, nullptr);
        }
    }
    release(handle);
    if (shutdown)
        shutdown(device.Get());
    Sleep(500);  // the capture DLL's writer finishes the folder
    const double n = (double)o.ow * o.oh;
    nlohmann::json out = {{"status", failures ? "failed: " + first_failure : "complete"},
                          {"feature", o.feature},
                          {"frames", o.frames},
                          {"failed_evaluates", failures},
                          {"render", {o.rw, o.rh}},
                          {"output", {o.ow, o.oh}},
                          {"last_output_mean", {sum[0] / n, sum[1] / n, sum[2] / n}}};
    std::printf("%s\n", out.dump().c_str());
    return failures ? 1 : 0;
}
