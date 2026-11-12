// nr-record's capture DLL for super resolution (SR) and ray reconstruction (RR) in D3D12 games (README.md): an NGX
// runtime, nr_record_sr.dll, that OptiScaler loads as a custom DLSS upscaler (OptiScaler/custom/nr-record/dlss/,
// Dx12Upscaler=nr-record). OptiScaler hands every upscaler call of the game, whether the game uses DLSS, FSR or XeSS,
// to it as one NGX DLSS evaluation, so one DLL records every game's SR inputs. It records them into nr-record folders
// (FORMAT.md "SR") and passes every call to an upscaler that writes the output (`forward` in nr_record.ini):
// - nvidia: NVIDIA's DLSS (and DLSS-RR) through NVIDIA's NGX core (_nvngx.dll, the driver's);
// - ffx: AMD's FSR through the FidelityFX API (ffx_dll: AMD's loader or amd_fidelityfx_dx12.dll), on any GPU;
// - xess: Intel's XeSS through libxess.dll (xess_dll), on any GPU;
// - a path: another DLL with NGX's D3D12 exports;
// - auto (the default): the upscaler OptiScaler.ini names (Dx12Upscaler: dlss, xess, or an FSR), else nvidia where
//   NVIDIA's NGX core is installed, else ffx; ray reconstruction always nvidia.
// OptiScaler sends calls here only while the user picks nr-record as the upscaler in its menu, so the game's upscaler
// is the user's own otherwise.
// F6 (record_key) starts and stops a recording; F9 (capture_key) records the next capture_frames real frames. Each real
// frame's inputs, and the forward's output after its call, are coded on the game's command list (D3D12Packer); the
// recording core (recorder.h) copies, compresses and writes them on its own queue and threads.

#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_params.h>

#include "record/d3d12_packer.h"
#include "record/ffx_upscaler.h"
#include "record/xess_upscaler.h"
#include "record/reader.h"
#include "record/capture/log_file.h"
#include "record/recorder.h"
#include "record/sr_roles.h"

struct NVSDK_NGX_Handle {
    unsigned int Id;
};

namespace {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------------------------------------------------
// Settings and log

struct Settings {
    int record_key = VK_F6, capture_key = VK_F9;
    unsigned capture_frames = 8;
    std::string record_dir, forward = "auto", nvidia_dlss, ffx_dll, xess_dll;
    bool record_output = true;
    unsigned record_ring = 4, record_threads = 0, record_queue_mb = 1024, record_max_seconds = 300;
    int record_zstd_level = 0;
    double record_free_gb = 50;
};

Settings g_settings;
std::string g_dir;  // this DLL's folder
std::mutex g_log_mutex;
std::FILE* g_log = nullptr;

void Log(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log)
        return;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
    std::fprintf(g_log, "[%s] %s\n", stamp, text);
    std::fflush(g_log);
}

int KeyCode(std::string v, int fallback) {
    for (char& c : v)
        c = (char)std::toupper((unsigned char)c);
    if (v.size() >= 2 && v[0] == 'F' && std::isdigit((unsigned char)v[1])) {
        const int n = std::atoi(v.c_str() + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }
    if (v.rfind("0X", 0) == 0)
        return (int)std::strtol(v.c_str() + 2, nullptr, 16);
    return v.empty() ? 0 : fallback;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// A path from nr_record.ini: absolute, or relative to this DLL's folder.
std::string Resolve(const std::string& path) {
    if (path.size() >= 2 && (path[1] == ':' || path[0] == '\\' || path[0] == '/'))
        return path;
    return g_dir + "\\" + path;
}

void ReadSettings() {
    std::ifstream ini(g_dir + "\\nr_record.ini");
    std::string line;
    while (std::getline(ini, line)) {
        line = Trim(line.substr(0, line.find_first_of(";#")));
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));
        // A value in quotes, as paths often are (record_dir = "D:\\Recordings"): the quotes are not part of it.
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = Trim(value.substr(1, value.size() - 2));
        try {
            if (key == "record_key") g_settings.record_key = KeyCode(value, VK_F6);
            else if (key == "capture_key") g_settings.capture_key = KeyCode(value, VK_F9);
            else if (key == "capture_frames") g_settings.capture_frames = std::max(1u, (unsigned)std::stoul(value));
            else if (key == "record_dir") g_settings.record_dir = value;
            else if (key == "record_output") g_settings.record_output = value != "0" && value != "false";
            else if (key == "record_ring") g_settings.record_ring = (unsigned)std::stoul(value);
            else if (key == "record_threads") g_settings.record_threads = (unsigned)std::stoul(value);
            else if (key == "record_queue_mb") g_settings.record_queue_mb = std::max(64u, (unsigned)std::stoul(value));
            else if (key == "record_max_seconds") g_settings.record_max_seconds = (unsigned)std::stoul(value);
            else if (key == "record_zstd_level") g_settings.record_zstd_level = std::stoi(value);
            else if (key == "record_free_gb") g_settings.record_free_gb = std::stod(value);
            else if (key == "forward") g_settings.forward = value;
            else if (key == "nvidia_dlss") g_settings.nvidia_dlss = value;
            else if (key == "ffx_dll") g_settings.ffx_dll = value;
            else if (key == "xess_dll") g_settings.xess_dll = value;
            // The frame generation DLL's keys, for one nr_record.ini in both folders.
            else if (key == "record_mode" || key == "record_present" || key == "nvidia_dlssg" ||
                     key == "max_generated") {}
            else Log("nr_record.ini: unknown key %s", key.c_str());
        } catch (...) {
            Log("nr_record.ini: %s = %s is not a valid value", key.c_str(), value.c_str());
        }
    }
}

std::once_flag g_init_once;

void Init() {
    std::call_once(g_init_once, [] {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&Init, &self);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(self, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
        g_log = record::OpenSessionLog(g_dir);
        ReadSettings();
        Log("nr-record SR capture DLL in %s; forward %s; record key 0x%x, capture key 0x%x (%u frames), recordings "
            "into %s", g_dir.c_str(), g_settings.forward.c_str(), g_settings.record_key, g_settings.capture_key,
            g_settings.capture_frames, record::DefaultRoot(g_settings.record_dir).c_str());
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// The NGX forward: NVIDIA's NGX core, or another DLL with NGX's D3D12 exports.

using InitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                                const NVSDK_NGX_FeatureCommonInfo*);
using InitProjectFn = NVSDK_NGX_Result(NVSDK_CONV*)(const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*,
                                                    ID3D12Device*, NVSDK_NGX_Version,
                                                    const NVSDK_NGX_FeatureCommonInfo*);
using Shutdown1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using ParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                               NVSDK_NGX_Handle**);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                 NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

struct Forward {
    HMODULE module = nullptr;
    bool nvidia = false;
    InitExtFn init_ext = nullptr;
    Shutdown1Fn shutdown1 = nullptr;
    ParametersFn capabilities = nullptr;
    DestroyParametersFn destroy_parameters = nullptr;
    CreateFn create = nullptr;
    ReleaseFn release = nullptr;
    EvaluateFn evaluate = nullptr;
    bool started = false;  // its Init ran here: its Shutdown runs here
    bool ready() const { return create && evaluate && release; }
};

Forward g_forward;
std::once_flag g_forward_once;
// What Init got, for starting the forward.
unsigned long long g_app = 0;
std::wstring g_data_path = L".";
ComPtr<ID3D12Device> g_device;
NVSDK_NGX_Version g_version = NVSDK_NGX_Version_API;
std::vector<std::wstring> g_paths;
std::vector<const wchar_t*> g_path_pointers;

// NVIDIA's NGX core: the one loaded in the process, else system32's. A loader hook (OptiScaler's) may answer a load
// of _nvngx.dll with another module, so the file name is checked.
HMODULE FindCore() {
    auto is_core = [](HMODULE m) {
        wchar_t path[MAX_PATH] = {};
        if (!m || !GetModuleFileNameW(m, path, MAX_PATH))
            return false;
        std::wstring name = path;
        name = name.substr(name.find_last_of(L"\\/") + 1);
        for (wchar_t& c : name)
            c = (wchar_t)towlower(c);
        return name == L"_nvngx.dll" && GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature") != nullptr;
    };
    HMODULE core = GetModuleHandleW(L"_nvngx.dll");
    if (is_core(core))
        return core;
    core = LoadLibraryW(L"_nvngx.dll");
    return is_core(core) ? core : nullptr;
}

void NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    Log("NGX: %s", text.c_str());
}

// OptiScaler's folder (custom/<name>/dlss's grandparent's parent), and the game's.
std::string OptiFolder() {
    return g_dir + "\\..\\..\\..";
}

std::string GameFolder() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const std::string game = exe;
    return game.substr(0, game.find_last_of("\\/"));
}

// The upscaler OptiScaler.ini names for D3D12 games as one of ours (nvidia, ffx, xess), or empty: the file is the one
// OptiScaler last saved, so with nr-record picked in the menu it still holds the user's own choice unless they saved
// since.
std::string IniUpscaler() {
    for (const std::string& folder : {OptiFolder(), OptiFolder() + "\\..", GameFolder()}) {
        std::ifstream ini(folder + "\\OptiScaler.ini");
        std::string line;
        while (std::getline(ini, line)) {
            line = Trim(line);
            if (line.rfind("Dx12Upscaler", 0) != 0 || line.find('=') == std::string::npos)
                continue;
            std::string v = Trim(line.substr(line.find('=') + 1));
            for (char& c : v)
                c = (char)std::tolower((unsigned char)c);
            if (v == "dlss")
                return "nvidia";
            if (v == "xess")
                return "xess";
            if (v == "fsr21" || v == "fsr22" || v == "fsr31" || v == "ffx")
                return "ffx";
            return "";
        }
    }
    return "";
}

// What upscales a feature (README.md "SR and RR": forward).
std::string ForwardKind(bool rr) {
    std::string kind = g_settings.forward;
    if (kind != "auto")
        return kind;
    if (rr)
        return "nvidia";  // only DLSS-RR does ray reconstruction
    kind = IniUpscaler();
    if (kind == "nvidia" && !FindCore())
        kind.clear();
    return !kind.empty() ? kind : FindCore() ? "nvidia" : "ffx";
}

// The NGX forward: NVIDIA's core, or forward's DLL where forward is a path. Loaded once.
Forward& LoadForward() {
    std::call_once(g_forward_once, [] {
        const std::string& kind = g_settings.forward;
        const bool path = !kind.empty() && kind != "auto" && kind != "nvidia" && kind != "ffx" && kind != "xess" &&
                          kind != "none";
        HMODULE m = nullptr;
        if (!path) {
            m = FindCore();
            if (!m) {
                Log("forward nvidia: NVIDIA's NGX core (_nvngx.dll) is not loaded and not in system32");
                return;
            }
            g_forward.nvidia = true;
        } else {
            const std::string file = Resolve(kind);
            m = LoadLibraryExA(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!m) {
                Log("forward: %s did not load (error %lu)", file.c_str(), GetLastError());
                return;
            }
        }
        auto get = [&](const char* name) { return GetProcAddress(m, name); };
        g_forward.module = m;
        g_forward.init_ext = (InitExtFn)get("NVSDK_NGX_D3D12_Init_Ext");
        g_forward.shutdown1 = (Shutdown1Fn)get("NVSDK_NGX_D3D12_Shutdown1");
        g_forward.capabilities = (ParametersFn)get("NVSDK_NGX_D3D12_GetCapabilityParameters");
        g_forward.destroy_parameters = (DestroyParametersFn)get("NVSDK_NGX_D3D12_DestroyParameters");
        g_forward.create = (CreateFn)get("NVSDK_NGX_D3D12_CreateFeature");
        g_forward.release = (ReleaseFn)get("NVSDK_NGX_D3D12_ReleaseFeature");
        g_forward.evaluate = (EvaluateFn)get("NVSDK_NGX_D3D12_EvaluateFeature");
        Log("forward: %s%s", g_forward.nvidia ? "NVIDIA's DLSS through the NGX core" : kind.c_str(),
            g_forward.ready() ? "" : ", which lacks NGX's D3D12 exports");
    });
    return g_forward;
}

// The forward started on the device: NVIDIA's core may already run for the game (OptiScaler starts it for DLSS-G and
// passthrough), then it is not started again.
bool StartForward(Forward& fw) {
    if (fw.started)
        return true;
    if (fw.nvidia && fw.capabilities) {
        NVSDK_NGX_Parameter* caps = nullptr;
        if (fw.capabilities(&caps) == NVSDK_NGX_Result_Success) {
            if (caps && fw.destroy_parameters)
                fw.destroy_parameters(caps);
            return true;
        }
    }
    if (!fw.init_ext || !g_device) {
        Log("forward: no Init_Ext or no device to start it on");
        return false;
    }
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = g_path_pointers.data();
    common.PathListInfo.Length = (unsigned)g_path_pointers.size();
    common.LoggingInfo.LoggingCallback = NgxLog;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    common.LoggingInfo.DisableOtherLoggingSinks = true;
    const NVSDK_NGX_Result r = fw.init_ext(g_app, g_data_path.c_str(), g_device.Get(), g_version, &common);
    if (NVSDK_NGX_FAILED(r)) {
        Log("forward: Init_Ext failed (0x%08x)", (unsigned)r);
        return false;
    }
    fw.started = true;
    Log("forward: started");
    return true;
}

// Init's search paths for the forward, with nvidia_dlss's folder and the game's.
void KeepPaths(const NVSDK_NGX_FeatureCommonInfo* info) {
    g_paths.clear();
    if (!g_settings.nvidia_dlss.empty()) {
        const std::string folder = Resolve(g_settings.nvidia_dlss);
        g_paths.emplace_back(folder.begin(), folder.end());
    }
    if (info && info->PathListInfo.Path)
        for (unsigned i = 0; i < info->PathListInfo.Length; i++)
            if (info->PathListInfo.Path[i])
                g_paths.emplace_back(info->PathListInfo.Path[i]);
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string exe_dir = exe;
    exe_dir = exe_dir.substr(0, exe_dir.find_last_of("\\/"));
    g_paths.emplace_back(exe_dir.begin(), exe_dir.end());
    g_path_pointers.clear();
    for (const std::wstring& p : g_paths)
        g_path_pointers.push_back(p.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------
// NGX parameters

template <typename T>
T Get(const NVSDK_NGX_Parameter* p, const char* name, T fallback) {
    T value = fallback;
    return p && p->Get(name, &value) == NVSDK_NGX_Result_Success ? value : fallback;
}

ID3D12Resource* Resource(const NVSDK_NGX_Parameter* p, const char* name) {
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) != NVSDK_NGX_Result_Success || !r) {
        void* pointer = nullptr;  // some hosts store resources as plain pointers
        if (p->Get(name, &pointer) == NVSDK_NGX_Result_Success)
            r = (ID3D12Resource*)pointer;
    }
    return r;
}

// The scalar parameters a DLSS or DLSS-RR evaluation (and OptiScaler's translation of FSR and XeSS) can carry, recorded
// per call as raw bits (FORMAT.md "params.bin"); the creation ones also go into record.json "create".
const char* const PARAMETERS[] = {
    "Width", "Height", "OutWidth", "OutHeight", "PerfQualityValue", "DLSS.Feature.Create.Flags",
    "DLSS.Enable.Output.Subrects", "CreationNodeMask", "VisibilityNodeMask", "DLSS.Hint.Render.Preset.DLAA",
    "DLSS.Hint.Render.Preset.Quality", "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
    "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality", "DLSS.Denoise.Mode",
    "DLSS.Roughness.Mode", "DLSS.Use.HW.Depth", "Reset", "Sharpness", "Jitter.Offset.X", "Jitter.Offset.Y",
    "MV.Scale.X", "MV.Scale.Y", "DLSS.Pre.Exposure", "DLSS.Exposure.Scale", "DLSS.Indicator.Invert.X.Axis",
    "DLSS.Indicator.Invert.Y.Axis", "DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height",
    "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y", "DLSS.Input.Depth.Subrect.Base.X",
    "DLSS.Input.Depth.Subrect.Base.Y", "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y",
    "DLSS.Input.Translucency.Subrect.Base.X", "DLSS.Input.Translucency.Subrect.Base.Y",
    "DLSS.Input.Bias.Current.Color.Subrect.Base.X", "DLSS.Input.Bias.Current.Color.Subrect.Base.Y",
    "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y", "FrameTimeDeltaInMsec",
    "DLSS.Input.DiffuseAlbedo.Subrect.Base.X", "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y",
    "DLSS.Input.SpecularAlbedo.Subrect.Base.X", "DLSS.Input.SpecularAlbedo.Subrect.Base.Y",
    "DLSS.Input.Normals.Subrect.Base.X", "DLSS.Input.Normals.Subrect.Base.Y", "DLSS.Input.Roughness.Subrect.Base.X",
    "DLSS.Input.Roughness.Subrect.Base.Y", "FSR.cameraNear", "FSR.cameraFar", "FSR.cameraFovAngleVertical",
    "FSR.frameTimeDelta", "FSR.viewSpaceToMetersFactor", "FSR.upscaleSize.width", "FSR.upscaleSize.height",
    "XeSS.MaxResponsiveMaskValue", "OptiScaler.SupportsUpscaleSize", "RayReconstruction.Hint.Render.Preset.DLAA",
    "RayReconstruction.Hint.Render.Preset.Quality", "RayReconstruction.Hint.Render.Preset.Balanced",
    "RayReconstruction.Hint.Render.Preset.Performance", "RayReconstruction.Hint.Render.Preset.UltraPerformance",
    "RayReconstruction.Hint.Render.Preset.UltraQuality",
};
const char* const MATRICES[] = {"WorldToViewMatrix", "ViewToClipMatrix", "InvViewProjectionMatrix",
                                "ClipToPrevClipMatrix"};

// ---------------------------------------------------------------------------------------------------------------------
// Features and recordings

struct Feature {
    NVSDK_NGX_Handle handle = {};
    NVSDK_NGX_Feature id = NVSDK_NGX_Feature_SuperSampling;
    NVSDK_NGX_Handle* forwarded = nullptr;  // the NGX forward's feature
    std::string kind;  // its forward (ForwardKind)
    std::unique_ptr<record::FfxUpscaler> ffx;
    std::unique_ptr<record::XessUpscaler> xess;
    bool upscaler_failed = false;
    nlohmann::json create;  // the creation parameters, as record.json "create"
    uint32_t width = 0, height = 0, out_width = 0, out_height = 0;
    uint64_t real_frames = 0;
    bool rr() const { return id == NVSDK_NGX_Feature_RayReconstruction; }
};

std::mutex g_mutex;
std::map<unsigned, std::unique_ptr<Feature>> g_features;
unsigned g_next_id = 1;
std::shared_ptr<record::D3D12Packer> g_packer;
ComPtr<ID3D12Device> g_packer_device;
std::unique_ptr<record::Recorder> g_recorder;
Feature* g_recording_feature = nullptr;
bool g_record_down = false, g_capture_down = false;

Feature* Find(const NVSDK_NGX_Handle* handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = handle ? g_features.find(handle->Id) : g_features.end();
    return it == g_features.end() ? nullptr : it->second.get();
}

// A parameter read both ways, as record.json "create" keeps it ({"u": ..., "f": ...}), or null if neither read works.
nlohmann::json Both(const NVSDK_NGX_Parameter* p, const char* name) {
    unsigned u = 0;
    float f = 0;
    nlohmann::json v = nlohmann::json::object();
    if (p->Get(name, &u) == NVSDK_NGX_Result_Success)
        v["u"] = u;
    if (p->Get(name, &f) == NVSDK_NGX_Result_Success)
        v["f"] = f;
    return v.empty() ? nlohmann::json() : v;
}

std::vector<record::InputDesc> Descs(const Feature& f) {
    std::vector<record::InputDesc> descs;
    for (const record::SrRole& r : record::SR_ROLES)
        if (!r.rr || f.rr())
            if (std::string(r.role) != "output" || g_settings.record_output)
                descs.push_back({r.role, r.required, ""});
    return descs;
}

// This real frame's textures in Descs' order (views kept in `views` for the call): inputs as DLSS takes them
// (NON_PIXEL_SHADER_RESOURCE), the output as a UAV.
std::vector<record::Input> Inputs(const Feature& f, const NVSDK_NGX_Parameter* p,
                                  std::vector<record::D3D12Texture>& views) {
    const std::vector<record::InputDesc> descs = Descs(f);
    views.assign(descs.size(), record::D3D12Texture{});
    std::vector<record::Input> inputs(descs.size());
    for (size_t i = 0; i < descs.size(); i++) {
        const record::SrRole* role = std::find_if(std::begin(record::SR_ROLES), std::end(record::SR_ROLES),
                                                  [&](const record::SrRole& r) { return descs[i].role == r.role; });
        ID3D12Resource* r = Resource(p, role->parameter);
        if (!r)
            continue;
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        views[i] = {r, descs[i].role == "output" ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                 : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        inputs[i] = {&views[i], (uint32_t)d.Format, (uint32_t)d.Width, d.Height};
    }
    return inputs;
}

std::unique_ptr<record::Recorder> StartRecording(Feature& f, ID3D12GraphicsCommandList* list,
                                                 const NVSDK_NGX_Parameter* p, unsigned frames, std::string& error) {
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) {
        error = "no device";
        return nullptr;
    }
    if (!g_packer || g_packer_device.Get() != device.Get()) {
        g_packer = record::D3D12Packer::Create(device.Get(), g_dir + "\\kernels", error);
        g_packer_device = device;
        if (!g_packer)
            return nullptr;
    }
    record::Config config;
    config.root = g_settings.record_dir;
    config.ring = g_settings.record_ring;
    config.zstd_level = g_settings.record_zstd_level;
    config.threads = g_settings.record_threads;
    config.queue_mb = g_settings.record_queue_mb;
    config.free_gb = g_settings.record_free_gb;
    config.max_seconds = frames ? 0 : g_settings.record_max_seconds;
    config.max_frames = frames;
    config.tag = std::string(f.rr() ? "rr" : "sr") + (frames ? "-capture" : "");
    config.log = [](record::Level, const std::string& line) { Log("%s", line.c_str()); };
    record::Session session;
    session.feature = f.rr() ? "rr" : "sr";
    session.source = f.rr() ? "ngx-dlssd" : "ngx-dlss";
    session.graphics_api = "d3d12";
    session.recorder = "nr-record SR capture DLL";
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    session.game = exe;
    session.width = f.out_width;
    session.height = f.out_height;
    session.fields = {{"create", f.create.dump()},
                      {"forward", nlohmann::json(f.kind).dump()},
                      {"hdr", (Get<unsigned>(p, "DLSS.Feature.Create.Flags", 0) & 1) ? "true" : "false"}};
    session.parameter_source = "ngx";
    session.parameters.assign(std::begin(PARAMETERS), std::end(PARAMETERS));
    session.matrices.assign(std::begin(MATRICES), std::end(MATRICES));
    std::vector<record::D3D12Texture> views;
    return record::Recorder::Start(device.Get(), g_packer, config, session, Descs(f), Inputs(f, p, views), error);
}

void RecordCall(const NVSDK_NGX_Parameter* p, uint64_t frame) {
    std::vector<uint8_t> values(record::CallBytes(std::size(PARAMETERS), std::size(MATRICES)) -
                                sizeof(record::CallHeader));
    record::CallHeader call;
    call.index = 1;
    call.count = 1;
    call.frame = frame;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    call.qpc = qpc.QuadPart;
    uint8_t* at = values.data();
    for (const char* name : PARAMETERS) {
        unsigned u = 0;
        float f = 0;
        uint32_t words[3] = {};
        words[0] = (p->Get(name, &u) == NVSDK_NGX_Result_Success ? 1u : 0u) |
                   (p->Get(name, &f) == NVSDK_NGX_Result_Success ? 2u : 0u);
        words[1] = u;
        std::memcpy(&words[2], &f, 4);
        std::memcpy(at, words, 12);
        at += 12;
    }
    for (const char* name : MATRICES) {
        void* pointer = nullptr;
        const uint32_t has = p->Get(name, &pointer) == NVSDK_NGX_Result_Success && pointer ? 1u : 0u;
        std::memcpy(at, &has, 4);
        if (has)
            std::memcpy(at + 4, pointer, 64);
        at += 68;
    }
    g_recorder->Call(call, values.data());
}

// After the forward's call: the keys, the start and the stop; then this frame's textures into the recording.
void RealFrame(Feature& f, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* p) {
    const bool record_now = g_settings.record_key && (GetAsyncKeyState(g_settings.record_key) & 0x8000) != 0;
    const bool capture_now = g_settings.capture_key && (GetAsyncKeyState(g_settings.capture_key) & 0x8000) != 0;
    const bool record_pressed = record_now && !g_record_down, capture_pressed = capture_now && !g_capture_down;
    g_record_down = record_now;
    g_capture_down = capture_now;
    std::vector<record::D3D12Texture> views;
    if (g_recorder && g_recording_feature == &f) {
        std::string why = record_pressed ? "record key pressed" : g_recorder->Due();
        if (!why.empty())
            g_recorder->Stop(why);
        if (!g_recorder->stopping()) {
            g_recorder->Frame(list, f.real_frames, Inputs(f, p, views));
            RecordCall(p, f.real_frames);
        }
        if (g_recorder->stopping() && g_recorder->finished()) {
            Log("RECORD stopped (%s): %s in %s", g_recorder->reason().c_str(), g_recorder->Totals().c_str(),
                g_recorder->dir().c_str());
            g_recorder.reset();
            g_recording_feature = nullptr;
            MessageBeep(MB_OK);
        }
        return;
    }
    if (g_recorder || (!record_pressed && !capture_pressed))
        return;
    std::string error;
    const unsigned frames = record_pressed ? 0 : g_settings.capture_frames;
    g_recorder = StartRecording(f, list, p, frames, error);
    if (!g_recorder) {
        Log("RECORD did not start: %s", error.c_str());
        MessageBeep(MB_ICONHAND);
        return;
    }
    g_recording_feature = &f;
    Log("RECORD started (%s, %s): %ux%u to %ux%u into %s; %s; ring %u x %.0f MiB, %u threads",
        frames ? "capture" : "recording", f.rr() ? "RR" : "SR", f.width, f.height, f.out_width, f.out_height,
        g_recorder->dir().c_str(), g_recorder->Layout().c_str(), g_recorder->slots(), g_recorder->slot_mib(),
        g_recorder->threads());
    MessageBeep(MB_OK);
    g_recorder->Frame(list, f.real_frames, Inputs(f, p, views));
    RecordCall(p, f.real_frames);
}

// The call's values in NGX's terms (reader.h SrCall), as the FidelityFX forward takes them.
record::SrCall CallOf(const Feature& f, const NVSDK_NGX_Parameter* p) {
    record::SrCall c;
    c.create_flags = Get<unsigned>(p, "DLSS.Feature.Create.Flags", f.create.value("DLSS.Feature.Create.Flags",
                                                                                   nlohmann::json::object())
                                                                       .value("u", 0u));
    c.hdr = c.create_flags & 1;
    c.motion_low_res = c.create_flags & 2;
    c.motion_jittered = c.create_flags & 4;
    c.depth_inverted = c.create_flags & 8;
    c.auto_exposure = c.create_flags & 64;
    c.reset = Get<unsigned>(p, "Reset", 0) != 0;
    c.jitter[0] = Get<float>(p, "Jitter.Offset.X", 0);
    c.jitter[1] = Get<float>(p, "Jitter.Offset.Y", 0);
    c.motion_scale[0] = Get<float>(p, "MV.Scale.X", 1);
    c.motion_scale[1] = Get<float>(p, "MV.Scale.Y", 1);
    c.render_size[0] = Get<unsigned>(p, "DLSS.Render.Subrect.Dimensions.Width", f.width);
    c.render_size[1] = Get<unsigned>(p, "DLSS.Render.Subrect.Dimensions.Height", f.height);
    c.output_size[0] = f.out_width;
    c.output_size[1] = f.out_height;
    c.pre_exposure = Get<float>(p, "DLSS.Pre.Exposure", 1);
    c.sharpness = Get<float>(p, "Sharpness", 0);
    c.near_plane = Get<float>(p, "FSR.cameraNear", 0);
    c.far_plane = Get<float>(p, "FSR.cameraFar", 0);
    c.fov = Get<float>(p, "FSR.cameraFovAngleVertical", 0);
    c.frame_time_ms = Get<float>(p, "FrameTimeDeltaInMsec", Get<float>(p, "FSR.frameTimeDelta", 0));
    return c;
}

// A DLL by name: the setting, else the first of `names` in amd/ beside this DLL (the package's FSR DLL: OptiScaler
// takes the first DLL of custom/<name>/dlss as the runtime, so no other DLL may sit beside this one), beside this DLL,
// in OptiScaler's folder or the game's that is not OptiScaler itself (which may sit in the game's folder under an
// upscaler's name).
std::string FindDll(const std::string& setting, std::initializer_list<const char*> names) {
    if (!setting.empty())
        return Resolve(setting);
    for (const std::string& folder : {g_dir + "\\amd", g_dir, OptiFolder(), GameFolder()})
        for (const char* name : names) {
            const std::string file = folder + "\\" + name;
            if (GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            HMODULE loaded = GetModuleHandleA(file.c_str());
            if (loaded && GetProcAddress(loaded, "NVSDK_NGX_D3D12_Init_Ext"))
                continue;
            return file;
        }
    return *names.begin();
}

// The DLL to load for `file`: OptiScaler takes over every DLL loaded under AMD's FidelityFX names or as libxess.dll (it
// hooks them as a game's upscaler inputs, and the calls would come back here), so such a DLL is loaded from a copy
// under a name of ours in %TEMP%\nr-record. A FidelityFX loader still loads its providers under AMD's names, so only
// single-DLL FSR (amd_fidelityfx_dx12.dll, FSR 3.1) works inside OptiScaler.
std::string PrivateCopy(const std::string& file) {
    std::string name = file.substr(file.find_last_of("\\/") + 1);
    for (char& c : name)
        c = (char)std::tolower((unsigned char)c);
    if (name.rfind("amd_fidelityfx", 0) != 0 && name != "libxess.dll")
        return file;
    if (name.find("loader") != std::string::npos || name.find("upscaler") != std::string::npos)
        Log("%s loads AMD's providers under AMD's names, which OptiScaler takes over: use amd_fidelityfx_dx12.dll",
            file.c_str());
    char temp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, temp);
    const std::string folder = std::string(temp) + "nr-record";
    CreateDirectoryA(folder.c_str(), nullptr);
    const std::string copy = folder + "\\nr_record_" + name;
    WIN32_FILE_ATTRIBUTE_DATA a = {}, b = {};
    const bool same = GetFileAttributesExA(file.c_str(), GetFileExInfoStandard, &a) &&
                      GetFileAttributesExA(copy.c_str(), GetFileExInfoStandard, &b) &&
                      a.nFileSizeLow == b.nFileSizeLow && a.nFileSizeHigh == b.nFileSizeHigh &&
                      CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
    if (!same && !CopyFileA(file.c_str(), copy.c_str(), FALSE)) {
        Log("cannot copy %s to %s (error %lu)", file.c_str(), copy.c_str(), GetLastError());
        return file;
    }
    return copy;
}

// forward = ffx or xess: FSR or XeSS on the frame (its context made at the first frame). False if it cannot run.
bool EvaluateUpscaler(Feature& f, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* p) {
    if (f.upscaler_failed)
        return false;
    const record::SrCall call = CallOf(f, p);
    std::string error;
    const bool xess = f.kind == "xess";
    if (!f.ffx && !f.xess) {
        ComPtr<ID3D12Device> device;
        list->GetDevice(IID_PPV_ARGS(&device));
        const std::string dll = PrivateCopy(xess ? FindDll(g_settings.xess_dll, {"libxess.dll"})
                                                 : FindDll(g_settings.ffx_dll, {"nr_record_ffx_dx12.dll",
                                                                                "amd_fidelityfx_dx12.dll"}));
        const std::wstring wide(dll.begin(), dll.end());
        bool ok;
        std::string version;
        if (xess) {
            f.xess = std::make_unique<record::XessUpscaler>();
            ok = f.xess->Load(wide, error) &&
                 f.xess->Create(device.Get(), f.width, f.height, f.out_width, f.out_height, call,
                                Resource(p, "ExposureTexture") != nullptr, Resource(p, "FSR.reactive") != nullptr,
                                error);
            version = ok ? f.xess->version() : "";
        } else {
            f.ffx = std::make_unique<record::FfxUpscaler>();
            ok = f.ffx->Load(wide, error) &&
                 f.ffx->Create(device.Get(), f.width, f.height, f.out_width, f.out_height, call, error);
            version = ok ? f.ffx->version() : "";
        }
        if (!ok) {
            Log("forward %s: %s: %s", f.kind.c_str(), dll.c_str(), error.c_str());
            f.upscaler_failed = true;
            return false;
        }
        Log("forward %s: %s, %s", f.kind.c_str(), dll.c_str(), version.c_str());
    }
    record::UpscaleTextures t;
    t.color = Resource(p, "Color");
    t.depth = Resource(p, "Depth");
    t.motion = Resource(p, "MotionVectors");
    t.exposure = Resource(p, "ExposureTexture");
    t.reactive = Resource(p, "FSR.reactive");
    t.transparency = Resource(p, "FSR.transparencyAndComposition");
    t.output = Resource(p, "Output");
    if (!(xess ? f.xess->Dispatch(list, t, call, error) : f.ffx->Dispatch(list, t, call, error))) {
        Log("forward %s: %s", f.kind.c_str(), error.c_str());
        return false;
    }
    return true;
}

template <typename F>
NVSDK_NGX_Result Guarded(const char* name, F&& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        Log("%s: %s", name, e.what());
    } catch (...) {
        Log("%s: an exception", name);
    }
    return NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_Result InitCommon(unsigned long long app, const wchar_t* path, ID3D12Device* device,
                            NVSDK_NGX_Version version, const NVSDK_NGX_FeatureCommonInfo* info) {
    Init();
    g_app = app;
    g_data_path = path ? path : L".";
    g_device = device;
    g_version = version;
    KeepPaths(info);
    return NVSDK_NGX_Result_Success;
}

}  // namespace

extern "C" {

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(unsigned long long app, const wchar_t* path,
                                                                           ID3D12Device* device,
                                                                           NVSDK_NGX_Version version,
                                                                           const NVSDK_NGX_FeatureCommonInfo* info) {
    return Guarded("Init_Ext", [&] { return InitCommon(app, path, device, version, info); });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init(unsigned long long app, const wchar_t* path,
                                                                       ID3D12Device* device,
                                                                       const NVSDK_NGX_FeatureCommonInfo* info,
                                                                       NVSDK_NGX_Version version) {
    return Guarded("Init", [&] { return InitCommon(app, path, device, version, info); });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* device) {
    return Guarded("Shutdown1", [&] {
        Init();
        // NVIDIA's core stays up for the process (its Shutdown1 faults under Wine, and a later Init finds it running);
        // a forward DLL is shut down.
        Forward& fw = g_forward;
        if (fw.started && fw.shutdown1 && !fw.nvidia) {
            fw.shutdown1(device ? device : g_device.Get());
            fw.started = false;
        }
        return NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown() {
    return NVSDK_NGX_D3D12_Shutdown1(nullptr);
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* list,
                                                                               NVSDK_NGX_Feature id,
                                                                               NVSDK_NGX_Parameter* p,
                                                                               NVSDK_NGX_Handle** out) {
    return Guarded("CreateFeature", [&] {
        Init();
        if ((id != NVSDK_NGX_Feature_SuperSampling && id != NVSDK_NGX_Feature_RayReconstruction) || !out || !p)
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        auto f = std::make_unique<Feature>();
        f->id = id;
        f->width = Get<unsigned>(p, "Width", 0);
        f->height = Get<unsigned>(p, "Height", 0);
        f->out_width = Get<unsigned>(p, "OutWidth", 0);
        f->out_height = Get<unsigned>(p, "OutHeight", 0);
        f->create = nlohmann::json::object();
        for (const char* name : PARAMETERS)
            if (nlohmann::json v = Both(p, name); !v.is_null())
                f->create[name] = v;
        f->kind = ForwardKind(f->rr());
        const bool upscaler = f->kind == "ffx" || f->kind == "xess";
        if (upscaler && f->rr()) {
            Log("forward %s: no ray reconstruction", f->kind.c_str());
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        }
        Forward& fw = upscaler || f->kind == "none" || f->kind.empty() ? g_forward : LoadForward();
        if (!upscaler && fw.ready()) {
            if (!StartForward(fw))
                return NVSDK_NGX_Result_FAIL_PlatformError;
            const NVSDK_NGX_Result r = fw.create(list, id, p, &f->forwarded);
            if (NVSDK_NGX_FAILED(r)) {
                Log("forward: CreateFeature failed (0x%08x)", (unsigned)r);
                return r;
            }
        } else if (!upscaler) {
            Log("CreateFeature: no forward runs the upscaler (forward = %s)", g_settings.forward.c_str());
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        }
        Log("%s feature through %s: %ux%u to %ux%u, flags 0x%x, quality %u",
            f->rr() ? "ray reconstruction" : "super resolution", f->kind.c_str(), f->width, f->height, f->out_width, f->out_height, Get<unsigned>(p, "DLSS.Feature.Create.Flags", 0),
            Get<unsigned>(p, "PerfQualityValue", 0));
        std::lock_guard<std::mutex> lock(g_mutex);
        f->handle.Id = g_next_id++;
        *out = &f->handle;
        g_features[f->handle.Id] = std::move(f);
        return NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* handle) {
    return Guarded("ReleaseFeature", [&] {
        Init();
        Feature* f = Find(handle);
        if (!f)
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        if (g_recorder && g_recording_feature == f) {
            const bool done = g_recorder->StopAndWait("upscaler feature released", 4000);
            Log("RECORD stopped (%s): %s in %s%s", g_recorder->reason().c_str(), g_recorder->Totals().c_str(),
                g_recorder->dir().c_str(), done ? "" : "; its writer had not finished");
            g_recorder.reset();
            g_recording_feature = nullptr;
        }
        Forward& fw = g_forward;
        if (f->forwarded && fw.release)
            fw.release(f->forwarded);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_features.erase(handle->Id);
        Log("upscaler feature released");
        return NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* list,
                                                                                 const NVSDK_NGX_Handle* handle,
                                                                                 NVSDK_NGX_Parameter* p,
                                                                                 PFN_NVSDK_NGX_ProgressCallback cb) {
    return Guarded("EvaluateFeature", [&] {
        Init();
        Feature* f = Find(handle);
        if (!f || !p || !list)
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        f->real_frames++;
        NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
        if (f->forwarded)
            r = g_forward.evaluate(list, f->forwarded, p, cb);
        else if (!EvaluateUpscaler(*f, list, p))
            r = NVSDK_NGX_Result_Fail;
        RealFrame(*f, list, p);
        return r;
    });
}

}  // extern "C"
