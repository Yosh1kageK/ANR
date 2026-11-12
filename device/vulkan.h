// A Vulkan device for NRI (Linux, Windows) with the extensions a caller asks for (swap chain, HDR, NGX, video decode)
// and every feature the GPU reports for them, wrapped into NRI. The device is created here and handed to NRI, because
// NRI's own device creation cannot enable cooperative matrices. caps records what the GPU offers.
//
//   nr::VulkanDevice vk;
//   vk.video_decode = true;  // optional
//   if (!vk.Create(/*window=*/true, {"VK_EXT_swapchain_colorspace"}, {}, VK_BINDING_OFFSETS, error)) ...
//   nri::Device* device = vk.nri;
//   ...
//   vk.Destroy();

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

namespace nr {

// What a Vulkan physical device offers, read before the device is created: identity and driver, subgroup and
// shared-memory limits, the SM count (VK_NV_shader_sm_builtins; 0 elsewhere), the cooperative-matrix formats, FP8 and
// other shader features.
struct VulkanCapabilities {
    std::string device_name, driver_name, driver_info;
    uint32_t vendor_id = 0, device_id = 0, driver_id = 0, driver_version = 0, api_version = 0;
    uint32_t subgroup_size = 0, subgroup_min = 0, subgroup_max = 0;
    bool required_subgroup_compute = false;  // compute pipelines may require a subgroup size (Vulkan 1.3)
    uint32_t max_shared_bytes = 0, max_invocations = 0;
    uint32_t sm_count = 0, warps_per_sm = 0;   // NVIDIA
    uint32_t compute_units = 0;                // AMD (VK_AMD_shader_core_properties2), else 0
    bool cooperative_matrix = false, float8 = false, float8_cooperative_matrix = false;
    bool float16 = false, int8 = false, storage16 = false, int64_atomics = false, shared_float_atomics = false;
    bool dot4x8_accelerated = false;
    struct Matrix {
        uint32_t m = 0, n = 0, k = 0;
        std::string a, b, c, result, scope;
        bool saturating = false;
    };
    std::vector<Matrix> matrices;
    std::string DriverVersion() const;  // NVIDIA's 10.8.8.6 bits as 620.70.06; others as Vulkan's 10.10.12
};

// The capabilities of a physical device of an instance made elsewhere (a game's, through vkd3d-proton's interop);
// get_instance_proc: its PFN_vkGetInstanceProcAddr. The extensions are those the physical device offers, not those a
// device enabled.
VulkanCapabilities QueryCapabilities(void* get_instance_proc, void* instance, void* physical_device);

struct VulkanDevice {
    void* instance = nullptr;        // VkInstance
    void* physical_device = nullptr; // VkPhysicalDevice
    void* device = nullptr;          // VkDevice
    void* get_instance_proc = nullptr;  // PFN_vkGetInstanceProcAddr
    uint32_t queue_family = 0;
    // Video decode (renderer/clips/video): set before Create to get queues of a video decode family (apart from the
    // graphics family) and the Vulkan video extensions the GPU has, which NRI's video extension needs. Without them
    // (or on a GPU with no video decode family) the device is created as before. Up to two queues, one per hardware
    // decoder where the GPU has two (the RTX 5090 does), so two videos decode at once.
    bool video_decode = false;
    uint32_t video_decode_queue_family = UINT32_MAX;  // after Create; UINT32_MAX: none
    uint32_t video_decode_queue_num = 0;              // after Create
    std::string loader_path;  // the Vulkan loader in use (empty: the system's)
    nri::Device* nri = nullptr;
    std::vector<std::string> instance_extensions, device_extensions;  // enabled
    VulkanCapabilities caps;  // of the chosen GPU

    // window: the instance and device extensions of a swap chain (Xlib, Wayland or Win32). The extra extensions are enabled where the
    // device supports them (NGX lists more than one driver may have); device_extensions_for(instance, physical device)
    // adds more once the GPU is chosen. binding_offsets: NRI's Vulkan binding offsets.
    bool Create(bool window, const std::vector<std::string>& extra_instance_extensions,
                const std::vector<std::string>& extra_device_extensions, const nri::VKBindingOffsets& binding_offsets,
                std::string& error,
                const std::function<std::vector<std::string>(void*, void*)>& device_extensions_for = nullptr);
    void Destroy();
};

}  // namespace nr
