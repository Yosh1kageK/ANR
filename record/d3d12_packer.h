// The recording core's GPU side in D3D12 games (recorder.h Packer): record_pack.slang's DXIL on the game's command
// list, natively and under vkd3d-proton alike. Textures are D3D12Texture, lists ID3D12GraphicsCommandList.
//
//   auto packer = record::D3D12Packer::Create(device, kernel_dir, error);   // kernel_dir: the three .dxil files
//   record::D3D12Texture t = {resource, D3D12_RESOURCE_STATE_COPY_DEST};     // as the game hands it over
//   ... record::Recorder::Start(device, packer, ...), then Frame(list, n, inputs with &t)
#pragma once

#include <d3d12.h>

#include <memory>
#include <string>

#include "record/recorder.h"

namespace record {

// A game texture and the state it is in during the call (and is left in).
struct D3D12Texture {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

// Channels of a texture format (FORMAT.md "kinds"), its Vulkan format number and the shader view format; false if
// the coding has no lossless view of it.
bool DescribeD3D12(const D3D12_RESOURCE_DESC& desc, Channels& out, DXGI_FORMAT& view, std::string& error);

class D3D12Packer : public Packer {
public:
    // kernel_dir holds record_pack.slang.record_init.dxil, .record_pack.dxil and .record_done.dxil.
    static std::shared_ptr<D3D12Packer> Create(ID3D12Device* device, const std::string& kernel_dir,
                                               std::string& error);
    ~D3D12Packer() override;
    bool Describe(const void* texture, Channels& out, std::string& error) override;
    bool Pack(void* list, uint32_t slot, ID3D12Resource* slot_buffer, uint32_t data_start,
              const std::vector<Texture>& textures, std::string& error) override;
    int Done(uint32_t slot, uint64_t& bytes, double& gpu_ms, ID3D12Fence*& fence, uint64_t& fence_value) override;
    bool SharedSlots() const override { return false; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace record
