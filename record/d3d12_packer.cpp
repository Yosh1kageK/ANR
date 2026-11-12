#include "record/d3d12_packer.h"

#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace record {
namespace {

using Microsoft::WRL::ComPtr;

// The coding's flags (record_pack.slang REC_*).
constexpr uint32_t REC_DEPTH = 1, REC_REFERENCE = 2, REC_SRGB = 4;
// Shader-visible descriptors: 2 SRVs (the texture and its reference) per texture, reused in a ring long enough that
// the GPU has finished with a pair before it comes round (TABLES / 8 real frames of 8 textures).
constexpr uint32_t TABLES = 1024;
constexpr uint32_t STATUS_BYTES = 8 * MAX_SLOTS, TIMES_BYTES = 16 * MAX_SLOTS;
// Vulkan format numbers (vulkan_core.h) that record.json names the texels by.
namespace vk {
constexpr uint32_t R8_UNORM = 9, R8G8B8A8_UNORM = 37, R8G8B8A8_SRGB = 43, B8G8R8A8_UNORM = 44, B8G8R8A8_SRGB = 50,
                   A2B10G10R10_UNORM_PACK32 = 64, R16_UNORM = 70, R16_SFLOAT = 76, R16G16_UNORM = 77,
                   R16G16_SFLOAT = 83, R16G16B16A16_UNORM = 91, R16G16B16A16_SFLOAT = 97, R32_SFLOAT = 100,
                   R32G32_SFLOAT = 103, D32_SFLOAT = 126, D32_SFLOAT_S8_UINT = 130;
}

std::vector<char> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

D3D12_RESOURCE_BARRIER Uav(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    return b;
}

}  // namespace

bool DescribeD3D12(const D3D12_RESOURCE_DESC& desc, Channels& out, DXGI_FORMAT& view, std::string& error) {
    out = {};
    out.width = (uint32_t)desc.Width;
    out.height = desc.Height;
    const bool depth = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    switch (desc.Format) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        view = DXGI_FORMAT_R10G10B10A2_UNORM, out.vk_format = vk::A2B10G10R10_UNORM_PACK32, out.channels = 4,
        out.kinds = 10 | 10 << 8 | 10 << 16 | 2 << 24; break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
        view = DXGI_FORMAT_R8G8B8A8_UNORM, out.vk_format = vk::R8G8B8A8_UNORM, out.channels = 4, out.kinds = 0x08080808;
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        view = desc.Format, out.vk_format = vk::R8G8B8A8_SRGB, out.srgb = true, out.channels = 4, out.kinds = 0x08080808;
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
        view = DXGI_FORMAT_B8G8R8A8_UNORM, out.vk_format = vk::B8G8R8A8_UNORM, out.channels = 4, out.kinds = 0x08080808;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        view = desc.Format, out.vk_format = vk::B8G8R8A8_SRGB, out.srgb = true, out.channels = 4, out.kinds = 0x08080808;
        break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        view = DXGI_FORMAT_R16G16B16A16_FLOAT, out.vk_format = vk::R16G16B16A16_SFLOAT, out.channels = 4,
        out.kinds = F16 * 0x01010101u; break;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        view = desc.Format, out.vk_format = vk::R16G16B16A16_UNORM, out.channels = 4, out.kinds = 0x10101010; break;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
        view = DXGI_FORMAT_R16G16_FLOAT, out.vk_format = vk::R16G16_SFLOAT, out.channels = 2, out.kinds = F16 | F16 << 8;
        break;
    case DXGI_FORMAT_R16G16_UNORM:
        view = desc.Format, out.vk_format = vk::R16G16_UNORM, out.channels = 2, out.kinds = 0x1010; break;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
        view = DXGI_FORMAT_R32G32_FLOAT, out.vk_format = vk::R32G32_SFLOAT, out.channels = 2, out.kinds = F32 | F32 << 8;
        break;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, out.vk_format = vk::D32_SFLOAT_S8_UINT, out.depth = true,
        out.channels = 1, out.kinds = F32; break;
    case DXGI_FORMAT_D32_FLOAT:
        view = DXGI_FORMAT_R32_FLOAT, out.vk_format = vk::D32_SFLOAT, out.depth = true, out.channels = 1,
        out.kinds = F32; break;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
        view = DXGI_FORMAT_R32_FLOAT, out.vk_format = depth ? vk::D32_SFLOAT : vk::R32_SFLOAT, out.depth = depth,
        out.channels = 1, out.kinds = F32; break;
    case DXGI_FORMAT_R16_FLOAT:
        view = desc.Format, out.vk_format = vk::R16_SFLOAT, out.channels = 1, out.kinds = F16; break;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM:
        view = DXGI_FORMAT_R16_UNORM, out.vk_format = vk::R16_UNORM, out.depth = depth || desc.Format == DXGI_FORMAT_D16_UNORM,
        out.channels = 1, out.kinds = 16; break;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM:
        view = DXGI_FORMAT_R8_UNORM, out.vk_format = vk::R8_UNORM, out.channels = 1, out.kinds = 8; break;
    default:
        error = "no lossless coding for DXGI format " + std::to_string((int)desc.Format);
        return false;
    }
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.Width > 16384) {
        error = "not a single-sample 2D texture up to 16384 wide";
        return false;
    }
    if (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) {
        error = "the texture denies shader reads";
        return false;
    }
    return true;
}

struct D3D12Packer::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> init, pack, done;
    ComPtr<ID3D12DescriptorHeap> heap;
    uint32_t increment = 0, next_table = 0;
    ComPtr<ID3D12Resource> status, readback;
    const uint8_t* mapped = nullptr;
    ComPtr<ID3D12QueryHeap> queries;
    uint64_t frequency = 0;
    uint32_t serial = 0;
    std::array<std::atomic<uint32_t>, MAX_SLOTS> expected = {};
};

std::shared_ptr<D3D12Packer> D3D12Packer::Create(ID3D12Device* device, const std::string& kernel_dir,
                                                 std::string& error) {
    auto p = std::shared_ptr<D3D12Packer>(new D3D12Packer());
    p->impl_ = std::make_unique<Impl>();
    Impl& m = *p->impl_;
    m.device = device;
    // b0: 8 constants; t0-t1: the texture pair's table; u0: the slot; u1: the status.
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0};
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor = {1, 0};
    D3D12_ROOT_SIGNATURE_DESC desc = {4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, message;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &message)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m.root)))) {
        error = "the root signature";
        return nullptr;
    }
    const std::pair<const char*, ComPtr<ID3D12PipelineState>*> kernels[] = {
        {"record_init", &m.init}, {"record_pack", &m.pack}, {"record_done", &m.done}};
    for (const auto& [name, pso] : kernels) {
        const std::string path = kernel_dir + "\\record_pack.slang." + name + ".dxil";
        const std::vector<char> code = ReadFile(path);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m.root.Get();
        pd.CS = {code.data(), code.size()};
        if (code.empty() || FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso->GetAddressOf())))) {
            error = code.empty() ? "no kernel " + path : "the pipeline of " + std::string(name);
            return nullptr;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * TABLES,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m.heap)))) {
        error = "the descriptor heap";
        return nullptr;
    }
    m.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Width = STATUS_BYTES;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const bool made =
        SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, IID_PPV_ARGS(&m.status))) &&
        (heap.Type = D3D12_HEAP_TYPE_READBACK, buffer.Width = STATUS_BYTES + TIMES_BYTES,
         buffer.Flags = D3D12_RESOURCE_FLAG_NONE,
         SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&m.readback))));
    void* data = nullptr;
    if (!made || FAILED(m.readback->Map(0, nullptr, &data))) {
        error = "the status buffers";
        return nullptr;
    }
    m.mapped = (const uint8_t*)data;
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * MAX_SLOTS, 0};
    ComPtr<ID3D12CommandQueue> queue;  // only for the timestamps' frequency
    D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m.queries))) &&
        SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))))
        queue->GetTimestampFrequency(&m.frequency);
    return p;
}

D3D12Packer::~D3D12Packer() {
    if (impl_ && impl_->readback && impl_->mapped)
        impl_->readback->Unmap(0, nullptr);
}

bool D3D12Packer::Describe(const void* texture, Channels& out, std::string& error) {
    const D3D12Texture& t = *(const D3D12Texture*)texture;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    return t.resource && DescribeD3D12(t.resource->GetDesc(), out, view, error);
}

bool D3D12Packer::Pack(void* list, uint32_t slot, ID3D12Resource* slot_buffer, uint32_t data_start,
                       const std::vector<Texture>& textures, std::string& error) {
    Impl& m = *impl_;
    auto* cl = (ID3D12GraphicsCommandList*)list;
    if (slot >= MAX_SLOTS || !cl) {
        error = "no such slot or list";
        return false;
    }
    // The textures (and references) in shader-readable states, each resource once.
    std::vector<D3D12_RESOURCE_BARRIER> into;
    auto readable = [&](const D3D12Texture* t) {
        if (!t || t->state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ||
            t->state == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE || (t->state & D3D12_RESOURCE_STATE_GENERIC_READ) ==
                                                                         D3D12_RESOURCE_STATE_GENERIC_READ)
            return;
        for (const D3D12_RESOURCE_BARRIER& b : into)
            if (b.Transition.pResource == t->resource)
                return;
        into.push_back(Transition(t->resource, t->state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    };
    struct Pass {
        uint32_t constants[8];
        D3D12_GPU_DESCRIPTOR_HANDLE table;
        uint32_t groups_x, groups_y;
    };
    std::vector<Pass> passes;
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = m.heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = m.heap->GetGPUDescriptorHandleForHeapStart();
    for (const Texture& t : textures) {
        const auto* tex = (const D3D12Texture*)t.texture;
        const auto* ref = (const D3D12Texture*)t.reference;
        Channels c;
        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN, ref_view = DXGI_FORMAT_UNKNOWN;
        if (!tex || !DescribeD3D12(tex->resource->GetDesc(), c, view, error))
            return false;
        Channels rc;
        if (ref && !DescribeD3D12(ref->resource->GetDesc(), rc, ref_view, error))
            return false;
        readable(tex);
        readable(ref);
        const uint32_t table = m.next_table;
        m.next_table = (m.next_table + 1) % TABLES;
        for (uint32_t i = 0; i < 2; i++) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = i == 0 || !ref ? view : ref_view;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE at = {cpu0.ptr + (SIZE_T)(2 * table + i) * m.increment};
            m.device->CreateShaderResourceView(i == 0 || !ref ? tex->resource : ref->resource, &sd, at);
        }
        const bool srgb_view = view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        Pass p = {{c.width, c.height, c.kinds,
                   (c.depth ? REC_DEPTH : 0u) | (ref ? REC_REFERENCE : 0u) | (srgb_view ? REC_SRGB : 0u), t.table,
                   c.channels, 0, 0},
                  {gpu0.ptr + (UINT64)(2 * table) * m.increment},
                  ((c.width + 7) / 8 + 31) / 32,
                  (c.height + 7) / 8};
        passes.push_back(p);
    }
    const uint32_t serial = ++m.serial ? m.serial : ++m.serial;  // 0 is never a serial
    if (!into.empty())
        cl->ResourceBarrier((UINT)into.size(), into.data());
    ID3D12DescriptorHeap* heaps[] = {m.heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(m.root.Get());
    cl->SetComputeRootUnorderedAccessView(2, slot_buffer->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(3, m.status->GetGPUVirtualAddress());
    if (m.queries)
        cl->EndQuery(m.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot);
    const uint32_t init[8] = {0, 0, 0, 0, 0, 0, data_start, slot};
    cl->SetPipelineState(m.init.Get());
    cl->SetComputeRoot32BitConstants(0, 8, init, 0);
    cl->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER slot_uav = Uav(slot_buffer);
    cl->ResourceBarrier(1, &slot_uav);
    cl->SetPipelineState(m.pack.Get());
    for (const Pass& p : passes) {
        cl->SetComputeRoot32BitConstants(0, 8, p.constants, 0);
        cl->SetComputeRootDescriptorTable(1, p.table);
        cl->Dispatch(p.groups_x, p.groups_y, 1);
    }
    cl->ResourceBarrier(1, &slot_uav);
    if (m.queries) {  // before the status, which tells the host the frame is done
        cl->EndQuery(m.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot + 1);
        cl->ResolveQueryData(m.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot, 2, m.readback.Get(),
                             STATUS_BYTES + 16ull * slot);
    }
    const uint32_t done[8] = {0, 0, 0, 0, 0, 0, serial, slot};
    cl->SetPipelineState(m.done.Get());
    cl->SetComputeRoot32BitConstants(0, 8, done, 0);
    cl->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER status[2] = {Uav(m.status.Get()), Transition(m.status.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                        D3D12_RESOURCE_STATE_COPY_SOURCE)};
    cl->ResourceBarrier(2, status);
    cl->CopyBufferRegion(m.readback.Get(), 8ull * slot, m.status.Get(), 8ull * slot, 8);
    std::swap(status[1].Transition.StateBefore, status[1].Transition.StateAfter);
    cl->ResourceBarrier(1, &status[1]);
    for (D3D12_RESOURCE_BARRIER& b : into)
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    if (!into.empty())
        cl->ResourceBarrier((UINT)into.size(), into.data());
    m.expected[slot] = serial;
    return true;
}

int D3D12Packer::Done(uint32_t slot, uint64_t& bytes, double& gpu_ms, ID3D12Fence*& fence, uint64_t& fence_value) {
    Impl& m = *impl_;
    fence = nullptr;
    fence_value = 0;
    if (slot >= MAX_SLOTS || !m.expected[slot])
        return -1;
    uint32_t words[2];
    std::memcpy(words, (const void*)(m.mapped + 8ull * slot), 8);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (words[1] != m.expected[slot])
        return 0;
    bytes = 4ull * words[0];
    gpu_ms = 0;
    uint64_t ticks[2];
    std::memcpy(ticks, m.mapped + STATUS_BYTES + 16ull * slot, 16);
    if (m.frequency && ticks[1] > ticks[0])
        gpu_ms = (double)(ticks[1] - ticks[0]) * 1000.0 / (double)m.frequency;
    m.expected[slot] = 0;
    return 1;
}

}  // namespace record
