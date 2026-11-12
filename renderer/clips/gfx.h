// The graphics device of the clip viewer and its offscreen exporter: an NRI device (Vulkan on Linux and Windows, Metal 4
// on macOS) with the core, helper, streamer and swap chain interfaces and the graphics queue, resource helpers, and the
// shader pipelines. Written like the renderer's own setup (renderer.cpp Initialize and its helpers); kept apart so
// the clip viewer, the study mode and the headless export share it without the renderer's scene.

#pragma once

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <NRIFramework.h>
#ifndef __APPLE__
#include "device/vulkan.h"
#endif

namespace renderer::clips {

#ifdef __APPLE__
inline constexpr bool METAL = true;
inline constexpr const char* SHADER_EXTENSION = ".dxil";
inline constexpr const char* SHADER_DIRECTORY = "renderer/clips/shaders_dxil/clips.slang.";
#else
inline constexpr bool METAL = false;
inline constexpr const char* SHADER_EXTENSION = ".spv";
inline constexpr const char* SHADER_DIRECTORY = "renderer/clips/shaders/clips.slang.";
#endif

// Buffers, textures, views and memory that live and die together.
struct GfxResources {
    std::vector<nri::Buffer*> buffers;
    std::vector<nri::Texture*> textures;
    std::vector<nri::Descriptor*> descriptors;
    std::vector<nri::Memory*> memory;
};

class Gfx {
public:
    ~Gfx() { Destroy(); }

    // window: the device will present to a window (the surface and HDR extensions). video_decode: also a video decode
    // queue and the Vulkan video extensions where the GPU has them (renderer/clips/video: GPU decode). False: the
    // error is in `error`.
    bool Create(bool window, std::string& error, bool video_decode = false);
    void Destroy();

    NRIInterface NRI = {};
    nri::Device* device = nullptr;
    nri::Queue* queue = nullptr;
    // One pool for every descriptor set of the viewer (a command buffer binds sets from one pool).
    nri::DescriptorPool* descriptor_pool = nullptr;
    bool CreateDescriptorPool(uint32_t sets, uint32_t textures);
    std::string device_name;
#ifndef __APPLE__
    nr::VulkanDevice vk;
#endif

    // Resources
    nri::Buffer* NewBuffer(GfxResources& r, uint64_t size, nri::BufferUsageBits usage, uint32_t stride = 0);
    nri::Texture* NewTexture(GfxResources& r, nri::Format format, uint32_t width, uint32_t height,
                             nri::TextureUsageBits usage);
    nri::Descriptor* TextureView(GfxResources& r, nri::Texture* texture, nri::TextureView type, nri::Format format);
    nri::Descriptor* BufferView(GfxResources& r, nri::Buffer* buffer, nri::BufferView type, uint64_t offset,
                                uint64_t size);
    // Binds memory to the buffers and textures from the given indices on.
    bool BindMemory(GfxResources& r, size_t first_buffer, size_t first_texture, nri::MemoryLocation location);
    void Release(GfxResources& r);

    // Pipelines: a fullscreen pass of the clips shaders for an attachment format (created on first use), and the layout
    // objects to free with the device.
    nri::Pipeline* Fullscreen(nri::PipelineLayout* layout, const char* fs, nri::Format format, bool blend = false);
    nri::PipelineLayout* NewLayout(const nri::PipelineLayoutDesc& desc);
    std::vector<char> ReadShader(const std::string& entry) const;

private:
    nri::Pipeline* GraphicsPipeline(nri::PipelineLayout* layout, const char* vs, const char* fs, nri::Format format,
                                    bool blend);
    std::vector<nri::Pipeline*> pipelines_;
    std::vector<nri::PipelineLayout*> layouts_;
    std::map<std::tuple<nri::PipelineLayout*, std::string, nri::Format, bool>, nri::Pipeline*> fullscreen_;
};

// A relative path from the workspace root (BUILD_WORKSPACE_DIRECTORY under bazel run) where one is known.
std::string WorkspacePath(const std::string& path);
// A path the user typed on the command line: relative to where they ran the command (BUILD_WORKING_DIRECTORY under
// bazel run, which starts the binary in its runfiles), else to the working directory.
std::string UserPath(const std::string& path);

}  // namespace renderer::clips
