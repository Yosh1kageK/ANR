// Intel's XeSS super resolution through libxess.dll (Intel's DLL, loaded at run time), driven by an SR call in NGX's
// terms (reader.h SrCall): the SR capture DLL's `xess` forward and nr-replay's xess generator.
//
//   record::XessUpscaler xess;
//   xess.Load(L"libxess.dll", error);
//   xess.Create(device, render_w, render_h, out_w, out_h, call, has_exposure, has_reactive, error);
//   xess.Dispatch(list, textures, call, error);   // per frame: inputs NON_PIXEL_SHADER_RESOURCE, output a UAV
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <string>

#include "record/ffx_upscaler.h"
#include "record/reader.h"

namespace record {

class XessUpscaler {
public:
    ~XessUpscaler();
    bool Load(const std::wstring& dll, std::string& error);
    // The context at the quality setting whose input range holds the render size, with the call's flags.
    bool Create(ID3D12Device* device, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                uint32_t output_height, const SrCall& call, bool exposure_texture, bool responsive_mask,
                std::string& error);
    // t.transparency is not used; t.reactive is XeSS's responsive pixel mask.
    bool Dispatch(ID3D12GraphicsCommandList* list, const UpscaleTextures& t, const SrCall& call, std::string& error);
    void Release();
    std::string version() const;  // libxess's, such as "2.1.1"
    int quality() const { return quality_; }

private:
    HMODULE module_ = nullptr;
    void *create_ = nullptr, *init_ = nullptr, *execute_ = nullptr, *destroy_ = nullptr, *velocity_ = nullptr,
         *optimal_ = nullptr, *version_ = nullptr;
    void* context_ = nullptr;
    int quality_ = 0;
};

}  // namespace record
