// DLSS-G's NGX D3D12 interface as a game's host drives it (generator.h MakeNgxGenerator): one feature, and per real
// frame one EvaluateFeature per generated frame with that call's recorded parameters.

#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params.h>

#include "record/replay/generator.h"
#include "record/replay/sha256.h"

namespace replay {

namespace {

// Application ID 1: a project ID needs NVIDIA's updater to map it, which Wine prefixes do not have.
constexpr unsigned long long APP_ID = 1;

using PFN_Init = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                                const NVSDK_NGX_FeatureCommonInfo*);
using PFN_Params = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using PFN_Populate = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using PFN_Create = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                  NVSDK_NGX_Handle**);
using PFN_Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                    NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using PFN_Release = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Hex(NVSDK_NGX_Result r) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", (unsigned)r);
    return text;
}

// A parameter map as a host keeps one for a provider's own exports: values by name, numbers readable as any number
// type, pointers as their own type or as void*.
class ParameterMap final : public NVSDK_NGX_Parameter {
public:
    void Set(const char* n, unsigned long long v) override { values_[n] = Number((double)v); }
    void Set(const char* n, float v) override { values_[n] = Number(v); }
    void Set(const char* n, double v) override { values_[n] = Number(v); }
    void Set(const char* n, unsigned int v) override { values_[n] = Number(v); }
    void Set(const char* n, int v) override { values_[n] = Number(v); }
    void Set(const char* n, ID3D11Resource* v) override { values_[n] = Pointer(v, D3D11); }
    void Set(const char* n, ID3D12Resource* v) override { values_[n] = Pointer(v, D3D12); }
    void Set(const char* n, void* v) override { values_[n] = Pointer(v, ANY); }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, double* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const override { return Read(n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** v) const override { return ReadPointer(n, v, D3D11); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const override { return ReadPointer(n, v, D3D12); }
    NVSDK_NGX_Result Get(const char* n, void** v) const override { return ReadPointer(n, v, ANY); }
    void Reset() override { values_.clear(); }

private:
    enum Kind { NUMBER, D3D11, D3D12, ANY };
    struct Value {
        Kind kind = NUMBER;
        double number = 0;  // every NGX number type converts exactly to and from double
        void* pointer = nullptr;
    };
    static Value Number(double v) { return {NUMBER, v, nullptr}; }
    static Value Pointer(void* p, Kind kind) { return {kind, 0, p}; }
    template <typename T>
    NVSDK_NGX_Result Read(const char* n, T* out) const {
        auto it = values_.find(n);
        if (!out || it == values_.end() || it->second.kind != NUMBER)
            return NVSDK_NGX_Result_Fail;
        *out = (T)it->second.number;
        return NVSDK_NGX_Result_Success;
    }
    template <typename T>
    NVSDK_NGX_Result ReadPointer(const char* n, T** out, Kind kind) const {
        auto it = values_.find(n);
        if (!out || it == values_.end() || it->second.kind == NUMBER ||
            (kind != ANY && it->second.kind != ANY && it->second.kind != kind))
            return NVSDK_NGX_Result_Fail;
        *out = (T*)it->second.pointer;
        return NVSDK_NGX_Result_Success;
    }
    std::unordered_map<std::string, Value> values_;
};

// The parameters NVIDIA's DLSS-G helpers (nvsdk_ngx_helpers_dlssg.h) set as floats. NGX parameters have no declared
// type, so the recording holds each value read both ways; these and values whose two readings differ go back as
// floats.
const std::set<std::string>& FloatNames() {
    static const std::set<std::string> names = [] {
        std::set<std::string> s;
        for (const char* n : {"JitterOffsetX", "JitterOffsetY", "MvecScaleX", "MvecScaleY", "CameraPinholeOffsetX",
                              "CameraPinholeOffsetY", "CameraPosX", "CameraPosY", "CameraPosZ", "CameraUpX",
                              "CameraUpY", "CameraUpZ", "CameraRightX", "CameraRightY", "CameraRightZ", "CameraFwdX",
                              "CameraFwdY", "CameraFwdZ", "CameraNear", "CameraFar", "CameraFOV",
                              "CameraAspectRatio", "MvecInvalidValue", "MinRelativeLinearDepthObjectSeparation",
                              "LinearizedDepth_Scale", "LinearizedDepth_NearFarPartition",
                              "BidirectionalDistortionFieldLowPrecision.Bias",
                              "BidirectionalDistortionFieldLowPrecision.Scale"})
            s.insert(std::string("DLSSG.") + n);
        return s;
    }();
    return names;
}

// Set by this host for every call, never from the recording.
const std::set<std::string>& HostNames() {
    static const std::set<std::string> names = {
        NVSDK_NGX_DLSSG_Parameter_Width, NVSDK_NGX_DLSSG_Parameter_Height, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat,
        NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,
        NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, NVSDK_NGX_DLSSG_Parameter_Reset};
    return names;
}

// A 4x4 inverse (Gauss-Jordan with partial pivoting).
std::array<float, 16> Inverse(const std::array<float, 16>& m) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++)
            a[r][c] = c < 4 ? m[4 * r + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        for (int k = 0; k < 8; k++)
            std::swap(a[c][k], a[pivot][k]);
        const double d = a[c][c] != 0 ? a[c][c] : 1e-30;
        for (int k = 0; k < 8; k++)
            a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; k++)
                    a[r][k] -= f * a[c][k];
            }
    }
    std::array<float, 16> out;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[4 * r + c] = (float)a[r][c + 4];
    return out;
}

void NVSDK_CONV Log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    std::fprintf(stderr, "NGX: %s\n", text.c_str());
}

// NVIDIA's NGX core: the folder the driver registers, else the system's search path (Wine prefixes: system32).
HMODULE LoadCore(std::string& path) {
    wchar_t folder[MAX_PATH] = {};
    DWORD bytes = sizeof(folder);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
                     RRF_RT_REG_SZ, nullptr, folder, &bytes) == ERROR_SUCCESS && folder[0]) {
        const std::wstring file = std::wstring(folder) + L"\\_nvngx.dll";
        if (HMODULE m = LoadLibraryW(file.c_str())) {
            path = Narrow(file);
            return m;
        }
    }
    HMODULE m = LoadLibraryW(L"_nvngx.dll");
    wchar_t file[MAX_PATH] = {};
    if (m && GetModuleFileNameW(m, file, MAX_PATH))
        path = Narrow(file);
    return m;
}

std::string ModulePath(HMODULE m) {
    wchar_t file[MAX_PATH] = {};
    return m && GetModuleFileNameW(m, file, MAX_PATH) ? Narrow(file) : std::string();
}

// A string of the DLL's version resource (FileDescription, ProductName ...), in its first language.
std::string VersionString(const std::vector<uint8_t>& info, const wchar_t* name) {
    struct Language {
        WORD language, codepage;
    }* languages = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", (void**)&languages, &bytes) ||
        bytes < sizeof(Language))
        return "";
    wchar_t key[128];
    std::swprintf(key, 128, L"\\StringFileInfo\\%04x%04x\\%ls", languages[0].language, languages[0].codepage, name);
    wchar_t* value = nullptr;
    return VerQueryValueW(info.data(), key, (void**)&value, &bytes) && value ? Narrow(value) : "";
}

std::vector<uint8_t> VersionInfo(const std::string& path) {
    const std::wstring w = Wide(path);
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(w.c_str(), &handle);
    std::vector<uint8_t> info(size);
    if (!size || !GetFileVersionInfoW(w.c_str(), 0, size, info.data()))
        info.clear();
    return info;
}

class NgxGenerator final : public FgGenerator {
public:
    NgxGenerator(bool core, std::string dll, std::string label, bool log)
        : core_(core), dll_(std::move(dll)), label_(std::move(label)), log_(log) {}

    bool Load(std::string& error) {
        std::error_code ec;
        const std::filesystem::path path = std::filesystem::absolute(dll_, ec);
        if (!std::filesystem::is_regular_file(path, ec)) {
            error = dll_ + ": no such file";
            return false;
        }
        dll_ = path.string();
        if (core_) {
            std::string name = path.filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](char c) { return (char)std::tolower(c); });
            if (name != "nvngx_dlssg.dll") {
                error = "--generator nvidia: NGX loads DLSS-G as nvngx_dlssg.dll from the DLL's folder; " + dll_ +
                        " has another name";
                return false;
            }
            module_ = LoadCore(core_path_);
            if (!module_) {
                error = "NVIDIA's NGX core (_nvngx.dll) is not installed: an NVIDIA driver is needed (under Wine, "
                        "the driver's NGX DLLs in the prefix's system32)";
                return false;
            }
        } else {
            module_ = LoadLibraryExW(Wide(dll_).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!module_) {
                error = "cannot load " + dll_ + " (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
        }
        auto get = [&](const char* name) { return (void*)GetProcAddress(module_, name); };
        init_ = (PFN_Init)get("NVSDK_NGX_D3D12_Init_Ext");
        create_ = (PFN_Create)get("NVSDK_NGX_D3D12_CreateFeature");
        evaluate_ = (PFN_Evaluate)get("NVSDK_NGX_D3D12_EvaluateFeature");
        release_ = (PFN_Release)get("NVSDK_NGX_D3D12_ReleaseFeature");
        populate_ = (PFN_Populate)get("NVSDK_NGX_D3D12_PopulateParameters_Impl");
        capabilities_ = (PFN_Params)get("NVSDK_NGX_D3D12_GetCapabilityParameters");
        allocate_ = (PFN_Params)get("NVSDK_NGX_D3D12_AllocateParameters");
        destroy_ = (PFN_Populate)get("NVSDK_NGX_D3D12_DestroyParameters");
        if (!init_ || !create_ || !evaluate_ || !release_ || (core_ && !capabilities_)) {
            error = (core_ ? core_path_ : dll_) + " lacks NGX's D3D12 exports (Init_Ext, CreateFeature, "
                                                  "EvaluateFeature, ReleaseFeature)";
            return false;
        }
        return true;
    }

    bool Create(const FgSetup& setup, ID3D12GraphicsCommandList* list, std::string& error) override {
        setup_ = setup;
        const std::wstring folder = Wide(std::filesystem::path(dll_).parent_path().string());
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const wchar_t* paths[] = {folder.c_str()};
        NVSDK_NGX_FeatureCommonInfo common = {};
        common.PathListInfo.Path = paths;
        common.PathListInfo.Length = 1;
        common.LoggingInfo.LoggingCallback = Log;
        common.LoggingInfo.MinimumLoggingLevel = log_ ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
        common.LoggingInfo.DisableOtherLoggingSinks = true;
        NVSDK_NGX_Result r = init_(APP_ID, temp, setup.device, NVSDK_NGX_Version_API, core_ ? &common : nullptr);
        if (NVSDK_NGX_FAILED(r)) {
            error = "NGX Init_Ext: " + Hex(r);
            return false;
        }
        unsigned max_count = 0;
        if (core_) {
            NVSDK_NGX_Parameter* caps = nullptr;
            int available = 0;
            if (NVSDK_NGX_FAILED(capabilities_(&caps)) || !caps) {
                error = "NGX GetCapabilityParameters failed";
                return false;
            }
            caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
            caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &max_count);
            if (!available) {
                error = "NVIDIA's DLSS-G is not available on this GPU and driver (FrameGeneration.Available 0)";
                return false;
            }
            if (!allocate_ || NVSDK_NGX_FAILED(allocate_(&params_)) || !params_)
                params_ = caps;
            else
                caps_ = caps;
        } else {
            params_ = &map_;
            if (populate_)
                populate_(params_);
            params_->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &max_count);
        }
        if (max_count && setup.multiplier - 1 > max_count) {
            error = "the generator makes up to " + std::to_string(max_count) + " generated frames per real frame "
                    "(MultiFrameCountMax); --multiplier " + std::to_string(setup.multiplier) + " needs " +
                    std::to_string(setup.multiplier - 1);
            return false;
        }
        max_count_ = max_count;
        params_->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        params_->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        params_->Set(NVSDK_NGX_Parameter_Width, setup.width);
        params_->Set(NVSDK_NGX_Parameter_Height, setup.height);
        params_->Set(NVSDK_NGX_DLSSG_Parameter_Width, setup.width);
        params_->Set(NVSDK_NGX_DLSSG_Parameter_Height, setup.height);
        params_->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, (unsigned)setup.backbuffer_format);
        params_->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, setup.render_width);
        params_->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, setup.render_height);
        params_->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
        r = create_(list, NVSDK_NGX_Feature_FrameGeneration, params_, &handle_);
        if (NVSDK_NGX_FAILED(r) || !handle_) {
            error = "NGX CreateFeature (frame generation): " + Hex(r);
            return false;
        }
        if (core_)  // the DLSS-G library NGX loaded, which may not be the one asked for
            loaded_ = ModulePath(GetModuleHandleW(L"nvngx_dlssg.dll"));
        for (size_t i = 0; i < setup.recording->parameters.size(); i++)
            index_[setup.recording->parameters[i]] = i;
        return true;
    }

    bool Evaluate(ID3D12GraphicsCommandList* list, const FgFrame& frame, const FgInputs& inputs,
                  const FgOutputs& outputs, std::string& error) override {
        const record::Recording& rec = *setup_.recording;
        // storage for each call's matrices, alive while the list runs: the recorded ones, then the defaults
        const size_t stride = rec.matrices.size() + DEFAULT_MATRICES.size();
        matrices_.assign(frame.count * stride, {});
        for (uint32_t i = 1; i <= frame.count; i++) {
            // the call for this index as recorded, or the frame's first call where the count differs
            const record::Call& c = i <= frame.calls.size() && frame.calls[i - 1]->count == frame.count
                                        ? *frame.calls[i - 1] : *frame.calls[0];
            NVSDK_NGX_Parameter* p = params_;
            p->Set(NVSDK_NGX_Parameter_Width, setup_.width);
            p->Set(NVSDK_NGX_Parameter_Height, setup_.height);
            p->Set(NVSDK_NGX_DLSSG_Parameter_Width, setup_.width);
            p->Set(NVSDK_NGX_DLSSG_Parameter_Height, setup_.height);
            p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, (unsigned)setup_.backbuffer_format);
            uint32_t recorded_reset = 0;
            for (size_t k = 0; k < rec.parameters.size(); k++) {
                const std::string& name = rec.parameters[k];
                const auto& v = c.values[k];  // has, unsigned, float bits
                float f;
                std::memcpy(&f, &v[2], 4);
                if (name == NVSDK_NGX_DLSSG_Parameter_Reset && (v[0] & 1))
                    recorded_reset = v[1];
                if (!v[0] || HostNames().count(name))
                    continue;
                const bool as_float = (v[0] & 2) && (!(v[0] & 1) || FloatNames().count(name) || f != (float)v[1]);
                if (as_float)
                    p->Set(name.c_str(), f);
                else
                    p->Set(name.c_str(), v[1]);
            }
            p->Set(NVSDK_NGX_DLSSG_Parameter_Reset, frame.reset ? 1u : recorded_reset);
            p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, frame.count);
            p->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, i);
            SetMatrices(c, (i - 1) * stride);
            static const std::pair<const char*, const char*> SLOTS[] = {
                {"backbuffer", NVSDK_NGX_DLSSG_Parameter_Backbuffer}, {"hudless", NVSDK_NGX_DLSSG_Parameter_HUDLess},
                {"ui", NVSDK_NGX_DLSSG_Parameter_UI}, {"ui_alpha", NVSDK_NGX_DLSSG_Parameter_UIAlpha},
                {"depth", NVSDK_NGX_DLSSG_Parameter_Depth}, {"motion", NVSDK_NGX_DLSSG_Parameter_MVecs}};
            for (const auto& [role, slot] : SLOTS) {
                auto it = inputs.textures.find(role);
                p->Set(slot, it == inputs.textures.end() ? (ID3D12Resource*)nullptr : it->second);
            }
            p->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, outputs.generated[i - 1]);
            p->Set(NVSDK_NGX_DLSSG_Parameter_OutputReal, outputs.real);
            const NVSDK_NGX_Result r = evaluate_(list, handle_, p, nullptr);
            if (NVSDK_NGX_FAILED(r)) {
                error = "NGX EvaluateFeature, generated frame " + std::to_string(i) + ": " + Hex(r);
                return false;
            }
        }
        return true;
    }

    void Release() override {
        if (handle_)
            release_(handle_);
        handle_ = nullptr;
        if (core_ && destroy_) {
            if (params_ && params_ != caps_)
                destroy_(params_);
            if (caps_)
                destroy_(caps_);
        }
        params_ = caps_ = nullptr;
    }

    nlohmann::json Describe() const override {
        const std::vector<uint8_t> info = VersionInfo(dll_);
        nlohmann::json out = {{"kind", core_ ? "nvidia" : "provider"}, {"dll", dll_}};
        const std::string version = FileVersion(dll_);
        const std::string description = info.empty() ? "" : VersionString(info, L"FileDescription");
        std::string label = label_;
        if (label.empty())
            label = (core_ ? std::string("DLSS-G") : description.empty() ? std::filesystem::path(dll_).stem().string()
                                                                          : description) +
                    (version.empty() ? "" : " " + version);
        out["label"] = label;
        out["file_version"] = version.empty() ? nlohmann::json() : nlohmann::json(version);
        out["description"] = description.empty() ? nlohmann::json() : nlohmann::json(description);
        out["sha256"] = FileSha256(dll_);
        if (core_) {
            out["ngx_core"] = core_path_;
            out["ngx_core_version"] = FileVersion(core_path_);
            out["loaded"] = loaded_.empty() ? nlohmann::json() : nlohmann::json(loaded_);
            if (!loaded_.empty() && FileSha256(loaded_) != out["sha256"].get<std::string>()) {
                out["loaded_file_version"] = FileVersion(loaded_);
                out["loaded_sha256"] = FileSha256(loaded_);
            }
        }
        out["multi_frame_count_max"] = max_count_;
        return out;
    }

private:
    // The NGX camera matrices; the identity where a recording has none, but the view-to-clip pair.
    inline static const std::vector<std::string> DEFAULT_MATRICES = {
        NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView,
        NVSDK_NGX_DLSSG_Parameter_ClipToLensClip, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip,
        NVSDK_NGX_DLSSG_Parameter_PrevClipToClip};

    float Value(const record::Call& c, const char* name, float fallback) const {
        auto it = index_.find(name);
        if (it == index_.end() || !(c.values[it->second][0] & 2))
            return fallback;
        float f;
        std::memcpy(&f, &c.values[it->second][2], 4);
        return f;
    }
    bool Has(const record::Call& c, const char* name) const {
        auto it = index_.find(name);
        return it != index_.end() && c.values[it->second][0] != 0;
    }

    // The call's matrices into storage from `at`, given as pointers. NVIDIA's DLSS-G needs a camera: where the game
    // gave none, it gets a perspective projection from the recorded planes, field of view and aspect, identities for
    // the others, and a camera basis along the axes. A provider gets what the game gave.
    void SetMatrices(const record::Call& c, size_t at) {
        const record::Recording& rec = *setup_.recording;
        std::set<std::string> given;
        for (size_t m = 0; m < rec.matrices.size(); m++)
            if (c.matrices[m].first) {
                matrices_[at + m] = c.matrices[m].second;
                params_->Set(rec.matrices[m].c_str(), (void*)matrices_[at + m].data());
                given.insert(rec.matrices[m]);
            }
        if (!core_)
            return;
        const float aspect_default = (float)setup_.width / (float)setup_.height;
        float fov = Value(c, NVSDK_NGX_DLSSG_Parameter_CameraFOV, 0), aspect =
            Value(c, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, 0);
        fov = fov > 0 ? fov : 1.0f;
        aspect = aspect > 0 ? aspect : aspect_default;
        float n = Value(c, NVSDK_NGX_DLSSG_Parameter_CameraNear, 0.1f), f =
            Value(c, NVSDK_NGX_DLSSG_Parameter_CameraFar, 1000.0f);
        auto it = index_.find(NVSDK_NGX_DLSSG_Parameter_DepthInverted);
        if (it != index_.end() && (c.values[it->second][0] & 1) && c.values[it->second][1])
            std::swap(n, f);
        const float y = 1.0f / std::tan(fov / 2);
        const std::array<float, 16> projection = {y / aspect, 0, 0, 0, 0, y, 0, 0, 0, 0, f / (f - n), 1,
                                                  0, 0, -n * f / (f - n), 0};
        const std::array<float, 16> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        size_t slot = at + rec.matrices.size();
        for (const std::string& name : DEFAULT_MATRICES) {
            if (given.count(name))
                continue;
            matrices_[slot] = name == NVSDK_NGX_DLSSG_Parameter_CameraViewToClip ? projection
                            : name == NVSDK_NGX_DLSSG_Parameter_ClipToCameraView ? Inverse(projection) : identity;
            params_->Set(name.c_str(), (void*)matrices_[slot].data());
            slot++;
        }
        if (!Has(c, NVSDK_NGX_DLSSG_Parameter_CameraFOV) || Value(c, NVSDK_NGX_DLSSG_Parameter_CameraFOV, 0) <= 0)
            params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, fov);
        if (!Has(c, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio) ||
            Value(c, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, 0) <= 0)
            params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, aspect);
        const bool basis = Value(c, NVSDK_NGX_DLSSG_Parameter_CameraUpY, 0) != 0 ||
                           Value(c, NVSDK_NGX_DLSSG_Parameter_CameraUpX, 0) != 0 ||
                           Value(c, NVSDK_NGX_DLSSG_Parameter_CameraUpZ, 0) != 0;
        if (!basis) {
            params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraUpY, 1.0f);
            params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraRightX, 1.0f);
            params_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFwdZ, 1.0f);
        }
    }

    bool core_;
    std::string dll_, label_, core_path_, loaded_;
    bool log_;
    HMODULE module_ = nullptr;
    PFN_Init init_ = nullptr;
    PFN_Create create_ = nullptr;
    PFN_Evaluate evaluate_ = nullptr;
    PFN_Release release_ = nullptr;
    PFN_Populate populate_ = nullptr, destroy_ = nullptr;
    PFN_Params capabilities_ = nullptr, allocate_ = nullptr;
    ParameterMap map_;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Parameter* caps_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    unsigned max_count_ = 0;
    FgSetup setup_;
    std::unordered_map<std::string, size_t> index_;
    std::vector<std::array<float, 16>> matrices_;
};

}  // namespace

std::unique_ptr<FgGenerator> MakeNgxGenerator(const std::string& kind, const std::string& dll,
                                              const std::string& label, bool log, std::string& error) {
    if (kind != "provider" && kind != "nvidia") {
        error = "--generator is provider or nvidia";
        return nullptr;
    }
    auto g = std::make_unique<NgxGenerator>(kind == "nvidia", dll, label, log);
    if (!g->Load(error))
        return nullptr;
    return g;
}

std::string FileVersion(const std::string& path) {
    const std::vector<uint8_t> info = VersionInfo(path);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT bytes = 0;
    if (info.empty() || !VerQueryValueW(info.data(), L"\\", (void**)&fixed, &bytes) || !fixed)
        return "";
    char text[64];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                  HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
    return text;
}

std::string FileSha256(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "";
    Sha256 sha;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), (std::streamsize)buffer.size());
        sha.Update(buffer.data(), (size_t)in.gcount());
    }
    return sha.Hex();
}

}  // namespace replay
