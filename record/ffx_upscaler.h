// AMD's FSR upscaler through the FidelityFX API (AMD's DLL, loaded at run time), driven by an SR call in NGX's terms
// (reader.h SrCall): the capture DLL's `ffx` forward and nr-replay's fsr generator.
//
//   record::FfxUpscaler fsr;
//   fsr.Load(L"amd_fidelityfx_loader_dx12.dll", error);   // or amd_fidelityfx_dx12.dll (FSR 3.1)
//   fsr.Create(device, render_w, render_h, out_w, out_h, call, error);
//   fsr.Dispatch(list, textures, call, error);   // per frame: inputs COMPUTE_READ, output UNORDERED_ACCESS
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <string>

#include "record/reader.h"

namespace record {

struct UpscaleTextures {
    ID3D12Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *exposure = nullptr, *reactive = nullptr,
                   *transparency = nullptr, *output = nullptr;
};

class FfxUpscaler {
public:
    ~FfxUpscaler();
    // A DLL with the FidelityFX API's exports: AMD's loader (amd_fidelityfx_loader_dx12.dll, with
    // amd_fidelityfx_upscaler_dx12.dll beside it) or FSR 3.1's amd_fidelityfx_dx12.dll.
    bool Load(const std::wstring& dll, std::string& error);
    // The context for these sizes and the call's flags (HDR, inverted depth, jittered or display-size motion,
    // automatic exposure).
    bool Create(ID3D12Device* device, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                uint32_t output_height, const SrCall& call, std::string& error);
    bool Dispatch(ID3D12GraphicsCommandList* list, const UpscaleTextures& t, const SrCall& call, std::string& error);
    void Release();
    bool created() const { return context_ != nullptr; }
    const std::string& version() const { return version_; }  // the provider's name, such as "FSR 3.1.5"
    HMODULE module() const { return module_; }

private:
    HMODULE module_ = nullptr;
    void* create_ = nullptr;
    void* destroy_ = nullptr;
    void* query_ = nullptr;
    void* dispatch_ = nullptr;
    void* context_ = nullptr;
    std::string version_;
};

}  // namespace record
