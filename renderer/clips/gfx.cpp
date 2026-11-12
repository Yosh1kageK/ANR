#include "renderer/clips/gfx.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "renderer/clips/process.h"
#include "device/log.h"
#include <spdlog/spdlog.h>

namespace renderer::clips {

std::string WorkspacePath(const std::string& path) {
    if (path.empty() || std::filesystem::path(path).is_absolute())
        return path;
    const char* workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
    return workspace ? std::string(workspace) + "/" + path : path;
}

std::string UserPath(const std::string& path) {
    if (path.empty() || std::filesystem::path(path).is_absolute())
        return path;
    const char* directory = std::getenv("BUILD_WORKING_DIRECTORY");
    return directory ? (std::filesystem::path(directory) / path).string() : path;
}

bool Gfx::Create(bool window, std::string& error, bool video_decode) {
#ifdef __APPLE__
    (void)window, (void)video_decode;
    // NRI's Metal 4 backend (macOS 26+).
    nri::DeviceCreationDesc creation = {};
    creation.graphicsAPI = nri::GraphicsAPI::METAL;
    creation.callbackInterface = nr::NriLogCallback();
    if (nri::nriCreateDevice(creation, device) != nri::Result::SUCCESS) {
        error = "NRI could not create a Metal device (Metal 4 needs macOS 26 and an Apple GPU)";
        return false;
    }
    if (!NRI_ENABLE_METAL_SHADER_CONVERTER)
        spdlog::critical("the shaders are DXIL: install Apple's Metal Shader Converter (/usr/local/lib) and build again");
#else
    // HDR swap chains: their color spaces and HDR metadata, where the system has them.
    const std::vector<std::string> instance_extensions = {"VK_EXT_swapchain_colorspace"};
    auto device_extensions = [](void*, void*) { return std::vector<std::string>{"VK_EXT_hdr_metadata"}; };
    vk.video_decode = video_decode;
    if (!vk.Create(window, instance_extensions, {}, VK_BINDING_OFFSETS, error, device_extensions))
        return false;
    device = vk.nri;
    device_name = vk.caps.device_name;
#endif
    if (nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), (nri::CoreInterface*)&NRI) != nri::Result::SUCCESS ||
        nri::nriGetInterface(*device, NRI_INTERFACE(nri::HelperInterface), (nri::HelperInterface*)&NRI) != nri::Result::SUCCESS ||
        nri::nriGetInterface(*device, NRI_INTERFACE(nri::StreamerInterface), (nri::StreamerInterface*)&NRI) !=
            nri::Result::SUCCESS) {
        error = "NRI interfaces are missing";
        return false;
    }
    if (window &&
        nri::nriGetInterface(*device, NRI_INTERFACE(nri::SwapChainInterface), (nri::SwapChainInterface*)&NRI) !=
            nri::Result::SUCCESS) {
        error = "NRI has no swap chain interface";
        return false;
    }
    if (NRI.GetQueue(*device, nri::QueueType::GRAPHICS, 0, queue) != nri::Result::SUCCESS) {
        error = "no graphics queue";
        return false;
    }
    return true;
}

bool Gfx::CreateDescriptorPool(uint32_t sets, uint32_t textures) {
    nri::DescriptorPoolDesc desc = {};
    desc.descriptorSetMaxNum = sets;
    desc.textureMaxNum = textures;
    return NRI.CreateDescriptorPool(*device, desc, descriptor_pool) == nri::Result::SUCCESS;
}

void Gfx::Destroy() {
    if (NRI.HasCore() && device) {
        NRI.DeviceWaitIdle(device);
        if (descriptor_pool)
            NRI.DestroyDescriptorPool(descriptor_pool);
        descriptor_pool = nullptr;
        for (nri::Pipeline* p : pipelines_)
            NRI.DestroyPipeline(p);
        for (nri::PipelineLayout* l : layouts_)
            NRI.DestroyPipelineLayout(l);
    }
    pipelines_.clear();
    layouts_.clear();
    fullscreen_.clear();
#ifdef __APPLE__
    if (device)
        nri::nriDestroyDevice(device);
#else
    vk.Destroy();
#endif
    device = nullptr;
    queue = nullptr;
    NRI = {};
}

nri::Buffer* Gfx::NewBuffer(GfxResources& r, uint64_t size, nri::BufferUsageBits usage, uint32_t stride) {
    nri::BufferDesc desc = {};
    desc.size = size;
    desc.structureStride = stride;
    desc.usage = usage;
    nri::Buffer* buffer = nullptr;
    if (NRI.CreateBuffer(*device, desc, buffer) != nri::Result::SUCCESS)
        return nullptr;
    r.buffers.push_back(buffer);
    return buffer;
}

nri::Texture* Gfx::NewTexture(GfxResources& r, nri::Format format, uint32_t width, uint32_t height,
                              nri::TextureUsageBits usage) {
    nri::TextureDesc desc = {};
    desc.type = nri::TextureType::TEXTURE_2D;
    desc.usage = usage;
    desc.format = format;
    desc.width = (nri::Dim_t)width;
    desc.height = (nri::Dim_t)height;
    desc.mipNum = 1;
    nri::Texture* texture = nullptr;
    if (NRI.CreateTexture(*device, desc, texture) != nri::Result::SUCCESS)
        return nullptr;
    r.textures.push_back(texture);
    return texture;
}

nri::Descriptor* Gfx::TextureView(GfxResources& r, nri::Texture* texture, nri::TextureView type, nri::Format format) {
    nri::TextureViewDesc desc = {texture, type, format};
    nri::Descriptor* view = nullptr;
    if (NRI.CreateTextureView(desc, view) != nri::Result::SUCCESS)
        return nullptr;
    r.descriptors.push_back(view);
    return view;
}

nri::Descriptor* Gfx::BufferView(GfxResources& r, nri::Buffer* buffer, nri::BufferView type, uint64_t offset,
                                 uint64_t size) {
    nri::BufferViewDesc desc = {buffer, type, offset, size};
    nri::Descriptor* view = nullptr;
    if (NRI.CreateBufferView(desc, view) != nri::Result::SUCCESS)
        return nullptr;
    r.descriptors.push_back(view);
    return view;
}

bool Gfx::BindMemory(GfxResources& r, size_t first_buffer, size_t first_texture, nri::MemoryLocation location) {
    nri::ResourceGroupDesc group = {};
    group.memoryLocation = location;
    group.buffers = r.buffers.data() + first_buffer;
    group.bufferNum = (uint32_t)(r.buffers.size() - first_buffer);
    group.textures = r.textures.data() + first_texture;
    group.textureNum = (uint32_t)(r.textures.size() - first_texture);
    const size_t first = r.memory.size();
    r.memory.resize(first + NRI.CalculateAllocationNumber(*device, group));
    return NRI.AllocateAndBindMemory(*device, group, r.memory.data() + first) == nri::Result::SUCCESS;
}

void Gfx::Release(GfxResources& r) {
    for (auto* d : r.descriptors)
        NRI.DestroyDescriptor(d);
    for (auto* t : r.textures)
        NRI.DestroyTexture(t);
    for (auto* b : r.buffers)
        NRI.DestroyBuffer(b);
    for (auto* m : r.memory)
        if (m)
            NRI.FreeMemory(m);
    r = GfxResources();
}

std::vector<char> Gfx::ReadShader(const std::string& entry) const {
    // Relative to the working directory (bazel run: the runfiles), else beside the executable's runfiles directory (the
    // binary started by hand from anywhere).
    const std::string relative = std::string(SHADER_DIRECTORY) + entry + SHADER_EXTENSION;
    std::vector<char> data;
    FILE* f = std::fopen(relative.c_str(), "rb");
    if (!f) {
        const std::string exe = ExecutablePath();
        for (const char* tree : {".runfiles/_main/", ".runfiles/"})
            if (!f && !exe.empty())
                f = std::fopen((exe + tree + relative).c_str(), "rb");
        if (!f && !exe.empty())
            f = std::fopen((std::filesystem::path(exe).parent_path() / relative).string().c_str(), "rb");
    }
    if (!f)
        return data;
    std::fseek(f, 0, SEEK_END);
    data.resize((size_t)std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    data.resize(std::fread(data.data(), 1, data.size(), f));
    std::fclose(f);
    return data;
}

nri::PipelineLayout* Gfx::NewLayout(const nri::PipelineLayoutDesc& desc) {
    nri::PipelineLayout* layout = nullptr;
    if (NRI.CreatePipelineLayout(*device, desc, layout) != nri::Result::SUCCESS)
        return nullptr;
    layouts_.push_back(layout);
    return layout;
}

nri::Pipeline* Gfx::GraphicsPipeline(nri::PipelineLayout* layout, const char* vs, const char* fs, nri::Format format,
                                     bool blend) {
    std::vector<char> vs_code = ReadShader(vs), fs_code = ReadShader(fs);
    if (vs_code.empty() || fs_code.empty()) {
        spdlog::critical("missing shader {} or {} ({}{}{})", vs, fs, SHADER_DIRECTORY, fs, SHADER_EXTENSION);
        return nullptr;
    }
    nri::ShaderDesc shaders[] = {
        {nri::StageBits::VERTEX_SHADER, vs_code.data(), vs_code.size(), METAL ? vs : "main"},
        {nri::StageBits::FRAGMENT_SHADER, fs_code.data(), fs_code.size(), METAL ? fs : "main"},
    };
    nri::ColorAttachmentDesc color = {};
    color.format = format;
    color.colorWriteMask = nri::ColorWriteBits::RGBA;
    if (blend) {
        color.blendEnabled = true;
        color.colorBlend = {nri::BlendFactor::SRC_ALPHA, nri::BlendFactor::ONE_MINUS_SRC_ALPHA, nri::BlendOp::ADD};
        color.alphaBlend = {nri::BlendFactor::ZERO, nri::BlendFactor::ONE, nri::BlendOp::ADD};
    }
    nri::InputAssemblyDesc assembly = {};
    assembly.topology = nri::Topology::TRIANGLE_LIST;
    nri::RasterizationDesc raster = {};
    raster.fillMode = nri::FillMode::SOLID;
    raster.cullMode = nri::CullMode::NONE;
    nri::MultisampleDesc multisample = {};
    multisample.sampleNum = 1;
    multisample.sampleMask = nri::ALL;
    nri::OutputMergerDesc merger = {};
    merger.colors = &color;
    merger.colorNum = 1;
    merger.depthStencilFormat = nri::Format::UNKNOWN;
    merger.depth.compareOp = nri::CompareOp::NONE;
    nri::GraphicsPipelineDesc desc = {};
    desc.pipelineLayout = layout;
    desc.inputAssembly = assembly;
    desc.rasterization = raster;
    desc.multisample = &multisample;
    desc.outputMerger = merger;
    desc.shaders = shaders;
    desc.shaderNum = 2;
    nri::Pipeline* pipeline = nullptr;
    if (NRI.CreateGraphicsPipeline(*device, desc, pipeline) != nri::Result::SUCCESS) {
        spdlog::critical("pipeline {} + {} failed", vs, fs);
        return nullptr;
    }
    pipelines_.push_back(pipeline);
    return pipeline;
}

nri::Pipeline* Gfx::Fullscreen(nri::PipelineLayout* layout, const char* fs, nri::Format format, bool blend) {
    const auto key = std::make_tuple(layout, std::string(fs), format, blend);
    auto found = fullscreen_.find(key);
    if (found != fullscreen_.end())
        return found->second;
    nri::Pipeline* pipeline = GraphicsPipeline(layout, "fullscreen_vs", fs, format, blend);
    fullscreen_[key] = pipeline;
    return pipeline;
}

}  // namespace renderer::clips
