// nr-record's capture DLL for D3D12 games (README.md): an NGX frame generation feature, nvngx_dlssg.dll, that
// OptiScaler loads as a custom DLSS-G provider (OptiScaler/custom/nr-record/dlssg/). It records the game's frame
// generation inputs into nr-record folders (FORMAT.md) and otherwise passes every call to another frame generation
// DLL (`forward` in nr_record.ini; `nvidia`: NVIDIA's DLSS-G through NVIDIA's NGX core) or, without one, shows the
// real frames only.
// - F6 (record_key) starts and stops a recording; F9 (capture_key) records the next capture_frames (8) real frames.
// - While it records, each generated slot is bypassed (OutputDisableInterpolation, the backbuffer in OutputReal), so
//   only real frames are shown, unless record_present = keep and a forward DLL generates them.
// - Each real frame's textures are coded on the game's command list (D3D12Packer); the recording core
//   (recorder.h) copies, compresses and writes them on its own queue and threads.
// The DLL holds no frame generation of its own.

#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
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

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params.h>

#include "record/d3d12_packer.h"
#include "record/recorder.h"

// The handle's layout as NGX and OptiScaler use it (the SDK's headers from 310 leave it opaque).
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
    std::string record_dir, record_mode = "interleaved", forward, nvidia_dlssg;
    bool record_bypass = true;
    unsigned record_ring = 4, record_threads = 0, record_queue_mb = 1024, record_max_seconds = 300;
    int record_zstd_level = 0;
    double record_free_gb = 50;
    unsigned max_generated = 5;
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

// nr_record.ini next to the DLL: key = value lines, ; or # comments.
void ReadSettings() {
    std::ifstream ini(g_dir + "\\nr_record.ini");
    std::string line;
    while (std::getline(ini, line)) {
        line = Trim(line.substr(0, line.find_first_of(";#")));
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        try {
            if (key == "record_key") g_settings.record_key = KeyCode(value, VK_F6);
            else if (key == "capture_key") g_settings.capture_key = KeyCode(value, VK_F9);
            else if (key == "capture_frames") g_settings.capture_frames = std::max(1u, (unsigned)std::stoul(value));
            else if (key == "record_dir") g_settings.record_dir = value;
            else if (key == "record_mode") g_settings.record_mode = value == "generated" ? "generated" : "interleaved";
            else if (key == "record_present") g_settings.record_bypass = value != "keep";
            else if (key == "record_ring") g_settings.record_ring = (unsigned)std::stoul(value);
            else if (key == "record_threads") g_settings.record_threads = (unsigned)std::stoul(value);
            else if (key == "record_queue_mb") g_settings.record_queue_mb = std::max(64u, (unsigned)std::stoul(value));
            else if (key == "record_max_seconds") g_settings.record_max_seconds = (unsigned)std::stoul(value);
            else if (key == "record_zstd_level") g_settings.record_zstd_level = std::stoi(value);
            else if (key == "record_free_gb") g_settings.record_free_gb = std::stod(value);
            else if (key == "forward") g_settings.forward = value;
            else if (key == "nvidia_dlssg") g_settings.nvidia_dlssg = value;
            else if (key == "max_generated") g_settings.max_generated = std::clamp((unsigned)std::stoul(value), 1u, 5u);
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
        g_log = std::fopen((g_dir + "\\nr_record.log").c_str(), "w");
        ReadSettings();
        Log("nr-record capture DLL in %s; record key 0x%x, capture key 0x%x (%u frames), recordings into %s",
            g_dir.c_str(), g_settings.record_key, g_settings.capture_key, g_settings.capture_frames,
            record::DefaultRoot(g_settings.record_dir).c_str());
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// The forward DLL: another NGX frame generation feature, called through its exports as OptiScaler calls ours.

using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                             const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using InitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                                const NVSDK_NGX_FeatureCommonInfo*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)();
using Shutdown1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using ScratchFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t*);
using RequirementsFn = NVSDK_NGX_Result(NVSDK_CONV*)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*,
                                                     NVSDK_NGX_FeatureRequirement*);
using PopulateFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                               NVSDK_NGX_Handle**);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                 NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

// forward = a DLL: its exports, all of them. forward = nvidia: NVIDIA's DLSS-G through NVIDIA's NGX core
// (_nvngx.dll), which the game or OptiScaler started for the game's DLSS: its CreateFeature, EvaluateFeature and
// ReleaseFeature only (its Init and Shutdown are the game's); if no one started it, Init_Ext starts it on the first
// feature, with the folder of nvidia_dlssg (NVIDIA's nvngx_dlssg.dll) and the game's in its search path.
using CoreInitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                                 const NVSDK_NGX_FeatureCommonInfo*);
using CapabilitiesFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);

struct Forward {
    HMODULE module = nullptr;
    bool nvidia = false;
    CoreInitFn core_init = nullptr;
    CapabilitiesFn capabilities = nullptr;
    DestroyParametersFn destroy_parameters = nullptr;
    Shutdown1Fn core_shutdown1 = nullptr;
    bool core_ours = false;  // started here: shut down here
    unsigned long long app = 0;
    std::wstring data_path = L".";
    ID3D12Device* device = nullptr;
    NVSDK_NGX_Version version = NVSDK_NGX_Version_API;
    InitFn init = nullptr;
    InitExtFn init_ext = nullptr;
    ShutdownFn shutdown = nullptr;
    Shutdown1Fn shutdown1 = nullptr;
    ScratchFn scratch = nullptr;
    RequirementsFn requirements = nullptr;
    PopulateFn populate = nullptr;
    CreateFn create = nullptr;
    ReleaseFn release = nullptr;
    EvaluateFn evaluate = nullptr;
    bool ready() const { return create && evaluate && release; }
};

Forward g_forward;
std::once_flag g_forward_once;

// NVIDIA's NGX core: the one loaded in the process, else system32's (the driver installs it there). A loader hook
// (OptiScaler's) may answer a load of _nvngx.dll with another module, so the file name is checked.
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

// forward = nvidia: the core running, started here if no one did. false if it cannot run.
bool StartCore(Forward& fw) {
    NVSDK_NGX_Parameter* caps = nullptr;
    if (fw.capabilities(&caps) == NVSDK_NGX_Result_Success) {
        if (caps && fw.destroy_parameters)
            fw.destroy_parameters(caps);
        return true;
    }
    if (!fw.device || !fw.core_init) {
        Log("forward nvidia: the NGX core is not started and there is no device to start it on");
        return false;
    }
    static std::vector<std::wstring> folders;
    static std::vector<const wchar_t*> pointers;
    folders.clear();
    pointers.clear();
    if (!g_settings.nvidia_dlssg.empty()) {
        const std::string folder = g_settings.nvidia_dlssg.substr(0, g_settings.nvidia_dlssg.find_last_of("\\/"));
        folders.push_back(std::wstring(folder.begin(), folder.end()));
    }
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string exe_dir = exe;
    exe_dir = exe_dir.substr(0, exe_dir.find_last_of("\\/"));
    folders.push_back(std::wstring(exe_dir.begin(), exe_dir.end()));
    for (const std::wstring& f : folders)
        pointers.push_back(f.c_str());
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = pointers.data();
    common.PathListInfo.Length = (unsigned)pointers.size();
    common.LoggingInfo.LoggingCallback = NgxLog;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    common.LoggingInfo.DisableOtherLoggingSinks = true;
    const NVSDK_NGX_Result r = fw.core_init(fw.app, fw.data_path.c_str(), fw.device, fw.version, &common);
    if (r != NVSDK_NGX_Result_Success) {
        Log("forward nvidia: the NGX core's Init_Ext failed (0x%08x)", (unsigned)r);
        return false;
    }
    fw.core_ours = true;
    Log("forward nvidia: started the NGX core");
    return true;
}

Forward& LoadForward() {
    std::call_once(g_forward_once, [] {
        if (g_settings.forward.empty())
            return;
        if (g_settings.forward == "nvidia") {
            HMODULE core = FindCore();
            if (!core) {
                Log("forward nvidia: NVIDIA's NGX core (_nvngx.dll) is not loaded and not in system32; the generated "
                    "slots are bypassed");
                return;
            }
            auto get = [&](const char* name) { return GetProcAddress(core, name); };
            g_forward.module = core;
            g_forward.nvidia = true;
            g_forward.core_init = (CoreInitFn)get("NVSDK_NGX_D3D12_Init_Ext");
            g_forward.capabilities = (CapabilitiesFn)get("NVSDK_NGX_D3D12_GetCapabilityParameters");
            g_forward.destroy_parameters = (DestroyParametersFn)get("NVSDK_NGX_D3D12_DestroyParameters");
            g_forward.core_shutdown1 = (Shutdown1Fn)get("NVSDK_NGX_D3D12_Shutdown1");
            g_forward.create = (CreateFn)get("NVSDK_NGX_D3D12_CreateFeature");
            g_forward.release = (ReleaseFn)get("NVSDK_NGX_D3D12_ReleaseFeature");
            g_forward.evaluate = (EvaluateFn)get("NVSDK_NGX_D3D12_EvaluateFeature");
            if (!g_forward.capabilities)
                g_forward.create = nullptr;
            Log("forward: NVIDIA's DLSS-G through the NGX core%s", g_forward.ready() ? "" : ", which lacks D3D12 exports");
            return;
        }
        std::string path = g_settings.forward;
        if (path.size() < 2 || (path[1] != ':' && path[0] != '\\' && path[0] != '/'))
            path = g_dir + "\\" + path;
        const std::wstring wide(path.begin(), path.end());
        g_forward.module = LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_forward.module) {
            Log("forward: %s did not load (error %lu); the generated slots are bypassed", path.c_str(),
                GetLastError());
            return;
        }
        auto get = [](const char* name) { return GetProcAddress(g_forward.module, name); };
        g_forward.init = (InitFn)get("NVSDK_NGX_D3D12_Init");
        g_forward.init_ext = (InitExtFn)get("NVSDK_NGX_D3D12_Init_Ext");
        g_forward.shutdown = (ShutdownFn)get("NVSDK_NGX_D3D12_Shutdown");
        g_forward.shutdown1 = (Shutdown1Fn)get("NVSDK_NGX_D3D12_Shutdown1");
        g_forward.scratch = (ScratchFn)get("NVSDK_NGX_D3D12_GetScratchBufferSize");
        g_forward.requirements = (RequirementsFn)get("NVSDK_NGX_D3D12_GetFeatureRequirements");
        g_forward.populate = (PopulateFn)get("NVSDK_NGX_D3D12_PopulateParameters_Impl");
        g_forward.create = (CreateFn)get("NVSDK_NGX_D3D12_CreateFeature");
        g_forward.release = (ReleaseFn)get("NVSDK_NGX_D3D12_ReleaseFeature");
        g_forward.evaluate = (EvaluateFn)get("NVSDK_NGX_D3D12_EvaluateFeature");
        Log("forward: %s %s", path.c_str(), g_forward.ready() ? "loaded" : "lacks DLSS-G's D3D12 exports");
    });
    return g_forward;
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

// The states Streamline hands DLSS-G's resources in (OptiScaler's own DLSS-G replacements take them so): the
// backbuffer shader-readable, its copies of the other inputs as copy destinations, the outputs as UAVs.
D3D12_RESOURCE_STATES InputState(const char* name) {
    const std::string n = name;
    if (n == NVSDK_NGX_DLSSG_Parameter_Backbuffer)
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (n == NVSDK_NGX_DLSSG_Parameter_OutputInterpolated || n == NVSDK_NGX_DLSSG_Parameter_OutputReal ||
        n == NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation)
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return D3D12_RESOURCE_STATE_COPY_DEST;
}

// Every DLSS-G parameter the SDK names, recorded per call as raw bits (FORMAT.md "params.bin").
const char* const PARAMETERS[] = {
    NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, NVSDK_NGX_DLSSG_Parameter_Width, NVSDK_NGX_DLSSG_Parameter_Height,
    NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,
    NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, NVSDK_NGX_DLSSG_Parameter_JitterOffsetY,
    NVSDK_NGX_DLSSG_Parameter_MvecScaleX, NVSDK_NGX_DLSSG_Parameter_MvecScaleY,
    NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY,
    NVSDK_NGX_DLSSG_Parameter_CameraPosX, NVSDK_NGX_DLSSG_Parameter_CameraPosY, NVSDK_NGX_DLSSG_Parameter_CameraPosZ,
    NVSDK_NGX_DLSSG_Parameter_CameraUpX, NVSDK_NGX_DLSSG_Parameter_CameraUpY, NVSDK_NGX_DLSSG_Parameter_CameraUpZ,
    NVSDK_NGX_DLSSG_Parameter_CameraRightX, NVSDK_NGX_DLSSG_Parameter_CameraRightY,
    NVSDK_NGX_DLSSG_Parameter_CameraRightZ, NVSDK_NGX_DLSSG_Parameter_CameraFwdX, NVSDK_NGX_DLSSG_Parameter_CameraFwdY,
    NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, NVSDK_NGX_DLSSG_Parameter_CameraNear, NVSDK_NGX_DLSSG_Parameter_CameraFar,
    NVSDK_NGX_DLSSG_Parameter_CameraFOV, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio,
    NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, NVSDK_NGX_DLSSG_Parameter_DepthInverted,
    NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, NVSDK_NGX_DLSSG_Parameter_Reset,
    NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames,
    NVSDK_NGX_DLSSG_Parameter_OrthoProjection, NVSDK_NGX_DLSSG_Parameter_InvertXAxis,
    NVSDK_NGX_DLSSG_Parameter_InvertYAxis, NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue,
    NVSDK_NGX_DLSSG_Parameter_MvecDilated, NVSDK_NGX_DLSSG_Parameter_MvecJittered,
    NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_Scale,
    NVSDK_NGX_DLSSG_Parameter_LinearizedDepth_NearFarPartition,
    NVSDK_NGX_DLSSG_Parameter_MinRelativeLinearDepthObjectSeparation,
    NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseY,
    NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight,
    NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseY,
    NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight,
    NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseY,
    NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight,
    NVSDK_NGX_DLSSG_Parameter_UISubrectBaseX, NVSDK_NGX_DLSSG_Parameter_UISubrectBaseY,
    NVSDK_NGX_DLSSG_Parameter_UISubrectWidth, NVSDK_NGX_DLSSG_Parameter_UISubrectHeight,
    NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectBaseY,
    NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectWidth, NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectHeight,
    NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseY,
    NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectBaseY,
    NVSDK_NGX_DLSSG_Parameter_InternalWidth, NVSDK_NGX_DLSSG_Parameter_InternalHeight,
    NVSDK_NGX_DLSSG_Parameter_DynamicResolution, NVSDK_NGX_DLSSG_Parameter_EvalFlags,
    NVSDK_NGX_DLSSG_Parameter_FullscreenMode, NVSDK_NGX_DLSSG_Parameter_TargetFrameRate,
    NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,
    NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
    NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled,
};
const char* const MATRICES[] = {
    NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView,
    NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip,
    NVSDK_NGX_DLSSG_Parameter_PrevClipToClip,
};
// The recorded textures (FORMAT.md FG roles) and their DLSS-G parameters.
const std::pair<const char*, const char*> ROLES[] = {
    {"backbuffer", NVSDK_NGX_DLSSG_Parameter_Backbuffer}, {"hudless", NVSDK_NGX_DLSSG_Parameter_HUDLess},
    {"ui", NVSDK_NGX_DLSSG_Parameter_UI},                 {"ui_alpha", NVSDK_NGX_DLSSG_Parameter_UIAlpha},
    {"depth", NVSDK_NGX_DLSSG_Parameter_Depth},           {"motion", NVSDK_NGX_DLSSG_Parameter_MVecs},
};

// The color encoding (record.json "encoding"): from the backbuffer's format and the HDR flag.
std::string EncodingOf(DXGI_FORMAT format, bool hdr) {
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return hdr ? "PQ10" : "sRGB8 (10-bit reads)";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return "scRGB16";
    default:
        return "sRGB8";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Features and recordings

struct Feature {
    NVSDK_NGX_Handle handle = {};
    NVSDK_NGX_Handle* forwarded = nullptr;  // the forward DLL's feature
    uint32_t width = 0, height = 0;
    DXGI_FORMAT backbuffer_format = DXGI_FORMAT_UNKNOWN;
    uint64_t real_frames = 0;
    ComPtr<ID3D12Resource> disable_values;  // upload: uint32 0 then 1
    bool logged_real = false;
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

// This real frame's textures for the core, in ROLES' order (views kept in `views` for the call).
std::vector<record::Input> Inputs(const NVSDK_NGX_Parameter* p, std::vector<record::D3D12Texture>& views) {
    views.assign(std::size(ROLES), record::D3D12Texture{});
    std::vector<record::Input> inputs(std::size(ROLES));
    for (size_t i = 0; i < std::size(ROLES); i++) {
        ID3D12Resource* r = Resource(p, ROLES[i].second);
        if (!r)
            continue;
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        views[i] = {r, InputState(ROLES[i].second)};
        inputs[i] = {&views[i], (uint32_t)d.Format, (uint32_t)d.Width, d.Height};
    }
    return inputs;
}

std::unique_ptr<record::Recorder> StartRecording(Feature& f, ID3D12GraphicsCommandList* list,
                                                 const NVSDK_NGX_Parameter* p, unsigned count, unsigned frames,
                                                 std::string& error) {
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
    config.tag = frames ? "capture" : "";
    config.log = [](record::Level, const std::string& line) { Log("%s", line.c_str()); };
    record::Session session;
    session.feature = "fg";
    session.source = "ngx-dlssg";
    session.graphics_api = "d3d12";
    session.recorder = "nr-record capture DLL";
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    session.game = exe;
    session.width = f.width;
    session.height = f.height;
    const bool hdr = Get<unsigned>(p, NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, 0) != 0;
    session.fields = {{"multiplier", std::to_string(count + 1)},
                      {"encoding", "\"" + EncodingOf(f.backbuffer_format, hdr) + "\""},
                      {"hdr", hdr ? "true" : "false"},
                      {"mode", "\"" + g_settings.record_mode + "\""}};
    session.parameter_source = "ngx";
    session.parameters.assign(std::begin(PARAMETERS), std::end(PARAMETERS));
    session.matrices.assign(std::begin(MATRICES), std::end(MATRICES));
    const std::vector<record::InputDesc> roles = {
        {"backbuffer", true, "hudless"}, {"hudless", false, ""}, {"ui", false, ""},
        {"ui_alpha", false, ""},         {"depth", true, ""},    {"motion", true, ""}};
    std::vector<record::D3D12Texture> views;
    return record::Recorder::Start(device.Get(), g_packer, config, session, roles, Inputs(p, views), error);
}

// Each call's DLSS-G parameters as raw bits.
void RecordCall(const NVSDK_NGX_Parameter* p, unsigned index, unsigned count, uint64_t frame) {
    std::vector<uint8_t> values(record::CallBytes(std::size(PARAMETERS), std::size(MATRICES)) -
                                sizeof(record::CallHeader));
    record::CallHeader call;
    call.index = index;
    call.count = count;
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

// The keys, the start and the stop at a real frame; then this frame's textures into the recording.
void RealFrame(Feature& f, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* p, unsigned count) {
    const bool record_now = g_settings.record_key && (GetAsyncKeyState(g_settings.record_key) & 0x8000) != 0;
    const bool capture_now = g_settings.capture_key && (GetAsyncKeyState(g_settings.capture_key) & 0x8000) != 0;
    const bool record_pressed = record_now && !g_record_down, capture_pressed = capture_now && !g_capture_down;
    g_record_down = record_now;
    g_capture_down = capture_now;
    if (g_recorder && g_recording_feature == &f) {
        std::string why = record_pressed ? "record key pressed" : g_recorder->Due();
        if (!why.empty())
            g_recorder->Stop(why);
        if (!g_recorder->stopping()) {
            std::vector<record::D3D12Texture> views;
            g_recorder->Frame(list, f.real_frames, Inputs(p, views));
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
    g_recorder = StartRecording(f, list, p, count, frames, error);
    if (!g_recorder) {
        Log("RECORD did not start: %s", error.c_str());
        MessageBeep(MB_ICONHAND);
        return;
    }
    g_recording_feature = &f;
    Log("RECORD started (%s): %uX into %s; %s; ring %u x %.0f MiB, %u threads", frames ? "capture" : "recording",
        count + 1, g_recorder->dir().c_str(), g_recorder->Layout().c_str(), g_recorder->slots(), g_recorder->slot_mib(),
        g_recorder->threads());
    MessageBeep(MB_OK);
    std::vector<record::D3D12Texture> views;
    g_recorder->Frame(list, f.real_frames, Inputs(p, views));
}

// Only the real frame: OutputDisableInterpolation 1 (the host drops the generated slot) and the backbuffer in
// OutputReal.
bool Bypass(Feature& f, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* p) {
    ID3D12Resource* disable = Resource(p, NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation);
    if (!disable)
        return false;
    if (!f.disable_values) {
        ComPtr<ID3D12Device> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 8;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* data = nullptr;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&f.disable_values))) ||
            FAILED(f.disable_values->Map(0, nullptr, &data)))
            return false;
        const uint32_t values[] = {0, 1};
        std::memcpy(data, values, sizeof(values));
        f.disable_values->Unmap(0, nullptr);
    }
    auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        list->ResourceBarrier(1, &barrier);
    };
    transition(disable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(disable, 0, f.disable_values.Get(), 4, 4);
    transition(disable, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* backbuffer = Resource(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer);
    ID3D12Resource* real = Resource(p, NVSDK_NGX_DLSSG_Parameter_OutputReal);
    if (backbuffer && real) {
        const D3D12_RESOURCE_DESC b = backbuffer->GetDesc(), r = real->GetDesc();
        const uint32_t x = Get<unsigned>(p, NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectBaseX, 0);
        const uint32_t y = Get<unsigned>(p, NVSDK_NGX_DLSSG_Parameter_BackbufferSubrectBaseY, 0);
        const bool fits = b.Format == r.Format && x + r.Width <= b.Width && y + r.Height <= b.Height;
        if (!f.logged_real) {
            f.logged_real = true;
            Log("OutputReal: %s", fits ? "the backbuffer is copied in" : "not written (format or size differs)");
        }
        if (fits) {
            transition(backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(real, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION to = {real, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            D3D12_TEXTURE_COPY_LOCATION from = {backbuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            const D3D12_BOX box = {x, y, 0, x + (UINT)r.Width, y + r.Height, 1};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            transition(real, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    return true;
}

// A C++ exception of ours does not reach the game: each export catches it, logs it and fails the call.
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

}  // namespace

extern "C" {

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init(unsigned long long app, const wchar_t* path,
                                                                       ID3D12Device* device,
                                                                       const NVSDK_NGX_FeatureCommonInfo* info,
                                                                       NVSDK_NGX_Version version) {
    return Guarded("Init", [&] {
        Init();
        Forward& fw = LoadForward();
        fw.app = app, fw.data_path = path ? path : L".", fw.device = device, fw.version = version;
        if (fw.init)
            return fw.init(app, path, device, info, version);
        if (fw.init_ext)
            return fw.init_ext(app, path, device, version, info);
        return NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(unsigned long long app, const wchar_t* path,
                                                                           ID3D12Device* device,
                                                                           NVSDK_NGX_Version version,
                                                                           const NVSDK_NGX_FeatureCommonInfo* info) {
    return Guarded("Init_Ext", [&] {
        Init();
        Forward& fw = LoadForward();
        fw.app = app, fw.data_path = path ? path : L".", fw.device = device, fw.version = version;
        if (fw.init_ext)
            return fw.init_ext(app, path, device, version, info);
        if (fw.init)
            return fw.init(app, path, device, info, version);
        return NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown() {
    return Guarded("Shutdown", [&] {
        Init();
        Forward& fw = LoadForward();
        if (fw.core_ours && fw.core_shutdown1 && fw.device) {  // the core started here
            fw.core_shutdown1(fw.device);
            fw.core_ours = false;
        }
        return fw.shutdown ? fw.shutdown() : NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* device) {
    return Guarded("Shutdown1", [&] {
        Init();
        Forward& fw = LoadForward();
        if (fw.core_ours && fw.core_shutdown1) {  // the core started here
            fw.core_shutdown1(device);
            fw.core_ours = false;
        }
        return fw.shutdown1 ? fw.shutdown1(device) : NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature feature,
                                                                                       const NVSDK_NGX_Parameter* p,
                                                                                       size_t* size) {
    return Guarded("GetScratchBufferSize", [&] {
        Init();
        Forward& fw = LoadForward();
        if (fw.scratch)
            return fw.scratch(feature, p, size);
        if (size)
            *size = 0;
        return NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* discovery, NVSDK_NGX_FeatureRequirement* out) {
    return Guarded("GetFeatureRequirements", [&] {
        Init();
        Forward& fw = LoadForward();
        if (fw.requirements)
            return fw.requirements(adapter, discovery, out);
        if (out)
            out->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        return NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_PopulateParameters_Impl(NVSDK_NGX_Parameter* p) {
    return Guarded("PopulateParameters_Impl", [&] {
        Init();
        Forward& fw = LoadForward();
        if (fw.populate)
            return fw.populate(p);
        if (!p)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        p->Set(NVSDK_NGX_Parameter_FrameGeneration_Available, 1);
        p->Set(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, (int)NVSDK_NGX_Result_Success);
        p->Set(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, 0);
        p->Set(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, 0);
        p->Set(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, 0);
        p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, g_settings.max_generated);
        return NVSDK_NGX_Result_Success;
    });
}

__declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* list,
                                                                               NVSDK_NGX_Feature id,
                                                                               NVSDK_NGX_Parameter* p,
                                                                               NVSDK_NGX_Handle** out) {
    return Guarded("CreateFeature", [&] {
        Init();
        if (id != NVSDK_NGX_Feature_FrameGeneration || !out || !p)
            return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        auto f = std::make_unique<Feature>();
        f->width = Get<unsigned>(p, NVSDK_NGX_Parameter_Width, 0);
        f->height = Get<unsigned>(p, NVSDK_NGX_Parameter_Height, 0);
        f->backbuffer_format = (DXGI_FORMAT)Get<unsigned>(p, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, 0);
        Forward& fw = LoadForward();
        if (fw.ready() && (!fw.nvidia || StartCore(fw))) {
            const NVSDK_NGX_Result r = fw.create(list, id, p, &f->forwarded);
            if (NVSDK_NGX_FAILED(r)) {
                Log("forward: CreateFeature failed (0x%08x); the generated slots are bypassed", (unsigned)r);
                f->forwarded = nullptr;
            }
        }
        Log("frame generation feature: %ux%u, backbuffer format %u%s", f->width, f->height,
            (unsigned)f->backbuffer_format, f->forwarded ? ", forwarded" : "");
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
            const bool done = g_recorder->StopAndWait("frame generation feature released", 4000);
            Log("RECORD stopped (%s): %s in %s%s", g_recorder->reason().c_str(), g_recorder->Totals().c_str(),
                g_recorder->dir().c_str(), done ? "" : "; its writer had not finished");
            g_recorder.reset();
            g_recording_feature = nullptr;
        }
        Forward& fw = LoadForward();
        if (f->forwarded && fw.release)
            fw.release(f->forwarded);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_features.erase(handle->Id);
        Log("frame generation feature released");
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
        const unsigned count = std::max(1u, Get<unsigned>(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 1));
        const unsigned index = Get<unsigned>(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1);
        if (index <= 1) {
            f->real_frames++;
            RealFrame(*f, list, p, count);
        }
        const bool recording = g_recorder && g_recording_feature == f && !g_recorder->stopping();
        if (recording)
            RecordCall(p, index, count, f->real_frames);
        Forward& fw = LoadForward();
        if (f->forwarded && !(recording && g_settings.record_bypass))
            return fw.evaluate(list, f->forwarded, p, cb);
        return Bypass(*f, list, p) ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    });
}

}  // extern "C"
