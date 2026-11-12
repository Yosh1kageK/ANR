// A frame generation implementation that nr-replay runs on a recording (README.md "Generators"). Each is a DLL that
// the user gives, loaded at run time and called through its public interface; nr-replay ships none of them.
//
// nr-replay uploads each recorded real frame's textures, then asks the generator for that frame's generated frames:
//
//   Create(setup, list)                  once, on the recording's size and formats
//   Evaluate(list, frame, inputs, outputs)  per real frame: generated frames 1 to frame.count into outputs
//   Describe()                           what ran, for replay.json
//
// nr-replay executes the list after each call, then reads the outputs back.
#pragma once

#include <d3d12.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "record/reader.h"

namespace replay {

struct FgSetup {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;  // the direct queue nr-replay executes the lists on
    const record::Recording* recording = nullptr;
    uint32_t width = 0, height = 0;                    // the output size (record.json width, height)
    DXGI_FORMAT backbuffer_format = DXGI_FORMAT_UNKNOWN;  // the outputs' format
    uint32_t render_width = 0, render_height = 0;      // the depth and motion size
    uint32_t multiplier = 2;                           // generated frames per real frame plus 1
};

// The real frame's textures by role (FORMAT.md "FG": backbuffer, hudless, ui, ui_alpha, depth, motion), in the states
// a DLSS-G host gives them: the backbuffer NON_PIXEL_SHADER_RESOURCE, the others COPY_DEST.
struct FgInputs {
    std::map<std::string, ID3D12Resource*> textures;
};

// Where the generated frames go: generated[i - 1] for index i, and real for the real frame's copy where the interface
// has one; UNORDERED_ACCESS, in the backbuffer's format.
struct FgOutputs {
    std::vector<ID3D12Resource*> generated;
    ID3D12Resource* real = nullptr;
};

struct FgFrame {
    // The real frame's recorded calls in order (with DLSS-G one per generated frame, index 1 to count) and their
    // parameters in the reader's convention.
    std::vector<const record::Call*> calls;
    std::vector<record::FgCall> typed;
    uint32_t count = 1;   // generated frames to make: the replay's multiplier - 1
    bool reset = false;   // the first frame of a run, and after dropped frames: no history
};

class FgGenerator {
public:
    virtual ~FgGenerator() = default;
    virtual bool Create(const FgSetup& setup, ID3D12GraphicsCommandList* list, std::string& error) = 0;
    virtual bool Evaluate(ID3D12GraphicsCommandList* list, const FgFrame& frame, const FgInputs& inputs,
                          const FgOutputs& outputs, std::string& error) = 0;
    virtual void Release() = 0;
    // kind, label, dll, file_version, sha256, and the DLL that ran if it differs
    virtual nlohmann::json Describe() const = 0;
};

// DLSS-G's NGX D3D12 interface. kind "provider": the DLL's own NVSDK_NGX_D3D12_* exports (a DLSS-G provider such as
// ANR FG's nvngx_dlssg.dll); "nvidia": NVIDIA's NGX core (_nvngx.dll, the driver's) with the DLL's folder in its
// search path, so that NVIDIA's DLSS-G from that nvngx_dlssg.dll runs.
std::unique_ptr<FgGenerator> MakeNgxGenerator(const std::string& kind, const std::string& dll,
                                              const std::string& label, bool log, std::string& error);

// The file version (a.b.c.d) and SHA-256 of a DLL; empty where not readable.
std::string FileVersion(const std::string& path);
std::string FileSha256(const std::string& path);

}  // namespace replay
