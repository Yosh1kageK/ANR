// nr-replay's SR and RR generators (sr_generator.h): NGX (NVIDIA's DLSS and DLSS-RR through the NGX core, or a DLL with
// NGX's exports), AMD's FSR through the FidelityFX API, and Intel's XeSS through libxess.dll.

#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_params.h>

#include "record/ffx_upscaler.h"
#include "record/xess_upscaler.h"
#include "record/replay/generator.h"
#include "record/replay/ngx_common.h"
#include "record/replay/sr_generator.h"
#include "record/sr_roles.h"

namespace replay {

namespace {

constexpr unsigned long long APP_ID = 1;

std::string Absolute(const std::string& dll, std::string& error) {
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::absolute(dll, ec);
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = dll + ": no such file";
        return "";
    }
    return path.string();
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (char)std::tolower(c); });
    return s;
}

// The generator's label: --label, else from the DLL's version resource.
std::string Label(const std::string& label, const std::string& name, const std::string& dll) {
    if (!label.empty())
        return label;
    const std::string version = FileVersion(dll);
    return name + (version.empty() ? "" : " " + version);
}

nlohmann::json DllJson(const std::string& kind, const std::string& label, const std::string& dll) {
    const std::vector<uint8_t> info = VersionInfo(dll);
    const std::string version = FileVersion(dll), description = VersionString(info, L"FileDescription");
    return {{"kind", kind},
            {"label", label},
            {"dll", dll},
            {"file_version", version.empty() ? nlohmann::json() : nlohmann::json(version)},
            {"description", description.empty() ? nlohmann::json() : nlohmann::json(description)},
            {"sha256", FileSha256(dll)}};
}

// NGX parameters that DLSS's helpers (nvsdk_ngx_helpers.h, _dlssd.h) and OptiScaler set as floats. NGX parameters
// have no declared type, so the recording holds each value read both ways; these and values whose two readings
// differ go back as floats.
bool IsFloat(const std::string& name) {
    static const std::set<std::string> names = {
        "Sharpness", "Jitter.Offset.X", "Jitter.Offset.Y", "MV.Scale.X", "MV.Scale.Y", "DLSS.Pre.Exposure",
        "DLSS.Exposure.Scale", "FrameTimeDeltaInMsec", "FSR.cameraNear", "FSR.cameraFar", "FSR.cameraFovAngleVertical",
        "FSR.frameTimeDelta", "FSR.viewSpaceToMetersFactor", "XeSS.MaxResponsiveMaskValue"};
    return names.count(name) != 0;
}

void SetValue(NVSDK_NGX_Parameter* p, const std::string& name, uint32_t has, uint32_t u, float f) {
    if (!has)
        return;
    if ((has & 2) && (!(has & 1) || IsFloat(name) || f != (float)u))
        p->Set(name.c_str(), f);
    else
        p->Set(name.c_str(), u);
}

// ---------------------------------------------------------------------------------------------------------------------
// NGX: NVIDIA's DLSS and DLSS-RR through the NGX core, or a DLL with NGX's exports.

class NgxSr final : public SrGenerator {
public:
    NgxSr(std::string kind, std::string dll, std::string label, bool log)
        : kind_(std::move(kind)), dll_(std::move(dll)), label_(std::move(label)), log_(log) {}

    bool Load(std::string& error) {
        dll_ = Absolute(dll_, error);
        if (dll_.empty())
            return false;
        core_ = kind_ != "ngx";
        if (core_) {
            const std::string want = kind_ == "dlssd" ? "nvngx_dlssd.dll" : "nvngx_dlss.dll";
            if (Lower(std::filesystem::path(dll_).filename().string()) != want) {
                error = "--generator " + kind_ + ": NGX loads it as " + want + " from the DLL's folder; " + dll_ +
                        " has another name";
                return false;
            }
            module_ = LoadNgxCore(core_path_);
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
        init_ = (PFN_NgxInit)get("NVSDK_NGX_D3D12_Init_Ext");
        shutdown_ = (PFN_NgxShutdown1)get("NVSDK_NGX_D3D12_Shutdown1");
        create_ = (PFN_NgxCreate)get("NVSDK_NGX_D3D12_CreateFeature");
        evaluate_ = (PFN_NgxEvaluate)get("NVSDK_NGX_D3D12_EvaluateFeature");
        release_ = (PFN_NgxRelease)get("NVSDK_NGX_D3D12_ReleaseFeature");
        capabilities_ = (PFN_NgxParams)get("NVSDK_NGX_D3D12_GetCapabilityParameters");
        allocate_ = (PFN_NgxParams)get("NVSDK_NGX_D3D12_AllocateParameters");
        destroy_ = (PFN_NgxPopulate)get("NVSDK_NGX_D3D12_DestroyParameters");
        if (!init_ || !create_ || !evaluate_ || !release_) {
            error = (core_ ? core_path_ : dll_) + " lacks NGX's D3D12 exports (Init_Ext, CreateFeature, "
                                                  "EvaluateFeature, ReleaseFeature)";
            return false;
        }
        return true;
    }

    bool Create(const SrSetup& setup, ID3D12GraphicsCommandList* list, std::string& error) override {
        setup_ = setup;
        const record::Recording& rec = *setup.recording;
        rr_ = rec.feature == "rr";
        if (rr_ && kind_ == "dlss") {
            error = "an RR recording replays through dlssd (DLSS-RR), not dlss";
            return false;
        }
        const std::wstring folder = Wide(std::filesystem::path(dll_).parent_path().string());
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const wchar_t* paths[] = {folder.c_str()};
        NVSDK_NGX_FeatureCommonInfo common = {};
        common.PathListInfo.Path = paths;
        common.PathListInfo.Length = 1;
        common.LoggingInfo.LoggingCallback = NgxLog;
        common.LoggingInfo.MinimumLoggingLevel = log_ ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
        common.LoggingInfo.DisableOtherLoggingSinks = true;
        NVSDK_NGX_Result r = init_(APP_ID, temp, setup.device, NVSDK_NGX_Version_API, &common);
        if (NVSDK_NGX_FAILED(r)) {
            error = "NGX Init_Ext: " + Hex(r);
            return false;
        }
        device_ = setup.device;
        if (core_) {
            NVSDK_NGX_Parameter* caps = nullptr;
            if (!capabilities_ || NVSDK_NGX_FAILED(capabilities_(&caps)) || !caps) {
                error = "NGX GetCapabilityParameters failed";
                return false;
            }
            int available = 0;
            caps->Get(rr_ ? NVSDK_NGX_Parameter_SuperSamplingDenoising_Available
                          : NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            if (!available) {
                error = std::string("NVIDIA's ") + (rr_ ? "DLSS-RR" : "DLSS") + " is not available on this GPU and "
                        "driver, or NGX did not find " + Lower(std::filesystem::path(dll_).filename().string());
                return false;
            }
            if (!allocate_ || NVSDK_NGX_FAILED(allocate_(&params_)) || !params_)
                params_ = caps;
            else
                caps_ = caps;
        } else {
            params_ = &map_;
        }
        // The recorded creation parameters, then this replay's sizes and preset.
        const nlohmann::json create = rec.meta.value("create", nlohmann::json::object());
        for (auto it = create.begin(); it != create.end(); ++it) {
            const uint32_t has = (it->contains("u") ? 1u : 0u) | (it->contains("f") ? 2u : 0u);
            SetValue(params_, it.key(), has, it->value("u", 0u), it->value("f", 0.0f));
        }
        params_->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        params_->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        params_->Set(NVSDK_NGX_Parameter_Width, setup.render_width);
        params_->Set(NVSDK_NGX_Parameter_Height, setup.render_height);
        params_->Set(NVSDK_NGX_Parameter_OutWidth, setup.width);
        params_->Set(NVSDK_NGX_Parameter_OutHeight, setup.height);
        params_->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, setup.first.create_flags);
        if (!create.contains("PerfQualityValue"))
            params_->Set(NVSDK_NGX_Parameter_PerfQualityValue, (unsigned)NVSDK_NGX_PerfQuality_Value_MaxQuality);
        if (setup.preset >= 0)
            for (const char* mode : {"DLAA", "Quality", "Balanced", "Performance", "UltraPerformance", "UltraQuality"})
                params_->Set(((rr_ ? "RayReconstruction.Hint.Render.Preset." : "DLSS.Hint.Render.Preset.") +
                              std::string(mode)).c_str(), (unsigned)setup.preset);
        r = create_(list, rr_ ? NVSDK_NGX_Feature_RayReconstruction : NVSDK_NGX_Feature_SuperSampling, params_,
                    &handle_);
        if (NVSDK_NGX_FAILED(r) || !handle_) {
            error = std::string("NGX CreateFeature (") + (rr_ ? "ray reconstruction" : "super resolution") +
                    "): " + Hex(r);
            return false;
        }
        if (core_)
            loaded_ = ModulePath(GetModuleHandleW(rr_ ? L"nvngx_dlssd.dll" : L"nvngx_dlss.dll"));
        return true;
    }

    bool Evaluate(ID3D12GraphicsCommandList* list, const SrFrame& frame, const SrInputs& inputs,
                  ID3D12Resource* output, std::string& error) override {
        const record::Recording& rec = *setup_.recording;
        NVSDK_NGX_Parameter* p = params_;
        for (size_t k = 0; k < rec.parameters.size(); k++) {
            const auto& v = frame.call->values[k];
            float f;
            std::memcpy(&f, &v[2], 4);
            SetValue(p, rec.parameters[k], v[0], v[1], f);
        }
        p->Set(NVSDK_NGX_Parameter_Reset, frame.reset || frame.typed.reset ? 1 : 0);
        for (const record::SrRole& role : record::SR_ROLES) {
            auto it = inputs.textures.find(role.role);
            if (std::string(role.role) != "output")
                p->Set(role.parameter, it == inputs.textures.end() ? (ID3D12Resource*)nullptr : it->second);
        }
        p->Set(NVSDK_NGX_Parameter_Output, output);
        // DLSS-RR needs the camera: the recorded matrices, else the identity and a perspective projection.
        matrices_.assign(rec.matrices.size() + 2, {});
        std::set<std::string> given;
        for (size_t m = 0; m < rec.matrices.size(); m++)
            if (frame.call->matrices[m].first) {
                matrices_[m] = frame.call->matrices[m].second;
                p->Set(rec.matrices[m].c_str(), (void*)matrices_[m].data());
                given.insert(rec.matrices[m]);
            }
        if (rr_ && core_) {
            const float y = 1.0f / std::tan(0.5f), a = (float)setup_.width / (float)setup_.height, n = 0.1f,
                        fa = 10000.0f;
            matrices_[rec.matrices.size()] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            matrices_[rec.matrices.size() + 1] = {y / a, 0, 0, 0, 0, y, 0, 0, 0, 0, fa / (fa - n), 1,
                                                  0, 0, -n * fa / (fa - n), 0};
            if (!given.count("WorldToViewMatrix"))
                p->Set("WorldToViewMatrix", (void*)matrices_[rec.matrices.size()].data());
            if (!given.count("ViewToClipMatrix"))
                p->Set("ViewToClipMatrix", (void*)matrices_[rec.matrices.size() + 1].data());
        }
        const NVSDK_NGX_Result r = evaluate_(list, handle_, p, nullptr);
        if (NVSDK_NGX_FAILED(r)) {
            error = "NGX EvaluateFeature: " + Hex(r);
            return false;
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
        // No Shutdown1: NVIDIA's core faults in it under Wine, and the process ends after the replay.
        if (shutdown_ && device_ && !core_)
            shutdown_(device_);
        device_ = nullptr;
    }

    nlohmann::json Describe() const override {
        const std::string name = kind_ == "dlss" ? "DLSS" : kind_ == "dlssd" ? "DLSS-RR"
                                 : std::filesystem::path(dll_).stem().string();
        nlohmann::json out = DllJson(kind_, Label(label_, name, dll_), dll_);
        if (core_) {
            out["ngx_core"] = core_path_;
            out["ngx_core_version"] = FileVersion(core_path_);
            out["loaded"] = loaded_.empty() ? nlohmann::json() : nlohmann::json(loaded_);
            if (!loaded_.empty() && FileSha256(loaded_) != out["sha256"].get<std::string>()) {
                out["loaded_file_version"] = FileVersion(loaded_);
                out["loaded_sha256"] = FileSha256(loaded_);
            }
        }
        if (setup_.preset >= 0)
            out["preset"] = std::string(1, (char)('A' + setup_.preset - 1));
        return out;
    }

private:
    std::string kind_, dll_, label_, core_path_, loaded_;
    bool log_, core_ = true, rr_ = false;
    HMODULE module_ = nullptr;
    PFN_NgxInit init_ = nullptr;
    PFN_NgxShutdown1 shutdown_ = nullptr;
    PFN_NgxCreate create_ = nullptr;
    PFN_NgxEvaluate evaluate_ = nullptr;
    PFN_NgxRelease release_ = nullptr;
    PFN_NgxParams capabilities_ = nullptr, allocate_ = nullptr;
    PFN_NgxPopulate destroy_ = nullptr;
    ParameterMap map_;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Parameter* caps_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    ID3D12Device* device_ = nullptr;
    SrSetup setup_;
    std::vector<std::array<float, 16>> matrices_;
};

// ---------------------------------------------------------------------------------------------------------------------
// FSR: AMD's FidelityFX API.

class FsrSr final : public SrGenerator {
public:
    FsrSr(std::string dll, std::string label) : dll_(std::move(dll)), label_(std::move(label)) {}

    bool Load(std::string& error) {
        dll_ = Absolute(dll_, error);
        return !dll_.empty() && fsr_.Load(Wide(dll_), error);
    }

    bool Create(const SrSetup& setup, ID3D12GraphicsCommandList*, std::string& error) override {
        if (setup.recording->feature == "rr") {
            error = "FSR has no ray reconstruction";
            return false;
        }
        return fsr_.Create(setup.device, setup.render_width, setup.render_height, setup.width, setup.height,
                           setup.first, error);
    }

    bool Evaluate(ID3D12GraphicsCommandList* list, const SrFrame& frame, const SrInputs& inputs,
                  ID3D12Resource* output, std::string& error) override {
        auto texture = [&](const char* role) {
            auto it = inputs.textures.find(role);
            return it == inputs.textures.end() ? nullptr : it->second;
        };
        record::UpscaleTextures t;
        t.color = texture("color");
        t.depth = texture("depth");
        t.motion = texture("motion");
        t.exposure = texture("exposure");
        t.reactive = texture("reactive");
        t.transparency = texture("transparency");
        t.output = output;
        record::SrCall call = frame.typed;
        call.reset = call.reset || frame.reset;
        return fsr_.Dispatch(list, t, call, error);
    }

    void Release() override { fsr_.Release(); }

    nlohmann::json Describe() const override {
        // The provider's version names the FSR that ran; the loader's file version does not.
        nlohmann::json out = DllJson("fsr", label_.empty() ? "FSR " + fsr_.version() : label_, dll_);
        out["provider"] = fsr_.version();
        return out;
    }

private:
    std::string dll_, label_;
    record::FfxUpscaler fsr_;
};

// ---------------------------------------------------------------------------------------------------------------------
// XeSS: Intel's libxess.dll.

class XessSr final : public SrGenerator {
public:
    XessSr(std::string dll, std::string label) : dll_(std::move(dll)), label_(std::move(label)) {}

    bool Load(std::string& error) {
        dll_ = Absolute(dll_, error);
        return !dll_.empty() && xess_.Load(Wide(dll_), error);
    }

    bool Create(const SrSetup& setup, ID3D12GraphicsCommandList*, std::string& error) override {
        if (setup.recording->feature == "rr") {
            error = "XeSS has no ray reconstruction";
            return false;
        }
        return xess_.Create(setup.device, setup.render_width, setup.render_height, setup.width, setup.height,
                            setup.first, setup.recording->Find("exposure") != nullptr,
                            setup.recording->Find("reactive") != nullptr, error);
    }

    bool Evaluate(ID3D12GraphicsCommandList* list, const SrFrame& frame, const SrInputs& inputs,
                  ID3D12Resource* output, std::string& error) override {
        auto texture = [&](const char* role) {
            auto it = inputs.textures.find(role);
            return it == inputs.textures.end() ? nullptr : it->second;
        };
        record::UpscaleTextures t;
        t.color = texture("color");
        t.depth = texture("depth");
        t.motion = texture("motion");
        t.exposure = texture("exposure");
        t.reactive = texture("reactive");
        t.output = output;
        record::SrCall call = frame.typed;
        call.reset = call.reset || frame.reset;
        return xess_.Dispatch(list, t, call, error);
    }

    void Release() override { xess_.Release(); }

    nlohmann::json Describe() const override {
        nlohmann::json out = DllJson("xess", Label(label_, "XeSS", dll_), dll_);
        out["xess_version"] = xess_.version();
        out["quality_setting"] = xess_.quality();
        return out;
    }

private:
    std::string dll_, label_;
    record::XessUpscaler xess_;
};

}  // namespace

std::unique_ptr<SrGenerator> MakeSrGenerator(const std::string& kind, const std::string& dll,
                                             const std::string& label, bool log, std::string& error) {
    if (kind == "dlss" || kind == "dlssd" || kind == "ngx") {
        auto g = std::make_unique<NgxSr>(kind, dll, label, log);
        return g->Load(error) ? std::move(g) : nullptr;
    }
    if (kind == "fsr") {
        auto g = std::make_unique<FsrSr>(dll, label);
        return g->Load(error) ? std::move(g) : nullptr;
    }
    if (kind == "xess") {
        auto g = std::make_unique<XessSr>(dll, label);
        return g->Load(error) ? std::move(g) : nullptr;
    }
    error = "--generator for SR and RR recordings is dlss, dlssd, ngx, fsr, xess or recorded";
    return nullptr;
}

}  // namespace replay
