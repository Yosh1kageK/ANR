// A super resolution (SR) or ray reconstruction (RR) implementation that nr-replay runs on an SR or RR recording
// (README.md "SR and RR generators"). Each is a DLL that the user gives, loaded at run time and called through its
// public interface; nr-replay ships none of them.
//
//   Create(setup, list)                   once, on the recording's sizes and creation parameters
//   Evaluate(list, frame, inputs, output)  per recorded frame: the upscaled (and for RR denoised) frame into output
//   Describe()                            what ran, for replay.json
#pragma once

#include <d3d12.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "record/reader.h"

namespace replay {

struct SrSetup {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    const record::Recording* recording = nullptr;
    uint32_t render_width = 0, render_height = 0;  // the color texture's size
    uint32_t width = 0, height = 0;                // the output's size
    DXGI_FORMAT output_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    record::SrCall first;  // the first replayed call: the creation flags and quality
    int preset = -1;       // --preset: DLSS's render preset hint (1 = A ...), -1: the recorded one
};

// The frame's textures by role (FORMAT.md "SR", "RR"), NON_PIXEL_SHADER_RESOURCE, as DLSS takes them.
struct SrInputs {
    std::map<std::string, ID3D12Resource*> textures;
};

struct SrFrame {
    const record::Call* call = nullptr;
    record::SrCall typed;
    bool reset = false;  // the first frame of a run, and after dropped frames: no history
};

class SrGenerator {
public:
    virtual ~SrGenerator() = default;
    virtual bool Create(const SrSetup& setup, ID3D12GraphicsCommandList* list, std::string& error) = 0;
    // output: UNORDERED_ACCESS, the output size, SrSetup::output_format.
    virtual bool Evaluate(ID3D12GraphicsCommandList* list, const SrFrame& frame, const SrInputs& inputs,
                          ID3D12Resource* output, std::string& error) = 0;
    virtual void Release() = 0;
    virtual nlohmann::json Describe() const = 0;
};

// kind dlss (NVIDIA's DLSS through the NGX core, from the folder of dll, an nvngx_dlss.dll), dlssd (DLSS-RR, an
// nvngx_dlssd.dll), ngx (a DLL with NGX's D3D12 exports and its own SR or RR), fsr (AMD's FidelityFX API DLL), xess
// (Intel's libxess.dll).
std::unique_ptr<SrGenerator> MakeSrGenerator(const std::string& kind, const std::string& dll,
                                             const std::string& label, bool log, std::string& error);

// nr-replay's entry for SR and RR recordings (sr_replay.cpp).
bool IsSrRecording(int argc, char** argv);
int RunSr(int argc, char** argv);

}  // namespace replay
