#include "device/vulkan.h"
#include "device/log.h"

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dlfcn.h>
#    include <limits.h>
#    include <stdlib.h>
#    include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <Extensions/NRIWrapperVK.h>

namespace nr {
namespace {

bool Has(const std::vector<VkExtensionProperties>& available, const std::string& name) {
    return std::any_of(available.begin(), available.end(),
                       [&](const VkExtensionProperties& e) { return name == e.extensionName; });
}

void AddUnique(std::vector<std::string>& list, const std::string& name) {
    if (std::find(list.begin(), list.end(), name) == list.end())
        list.push_back(name);
}

#ifdef _WIN32
constexpr const char* SYSTEM_LOADER = "vulkan-1.dll";

void* OpenLibrary(const std::string& path) {
    return (void*)LoadLibraryA(path.c_str());
}

void* Symbol(void* library, const char* name) {
    return (void*)GetProcAddress((HMODULE)library, name);
}
#else
constexpr const char* SYSTEM_LOADER = "libvulkan.so.1";

void* OpenLibrary(const std::string& path) {
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void* Symbol(void* library, const char* name) {
    return dlsym(library, name);
}
#endif

// Linux: the Vulkan loader built from source (@vulkan_loader) in this binary's runfiles. Windows: the system's
// (the GPU driver installs it). NR_VULKAN_LOADER overrides both; the system's loader is the last resort.
std::string LoaderPath() {
    const char* forced = std::getenv("NR_VULKAN_LOADER");
    if (forced && *forced)
        return forced;
#ifdef _WIN32
    return "";
#else
    const std::string relative = "+http_archive+vulkan_loader/libvulkan.so.1";
    std::vector<std::string> candidates;
    char exe[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
        candidates.push_back(std::string(exe, (size_t)n) + ".runfiles/" + relative);
    if (const char* runfiles = std::getenv("RUNFILES_DIR"))
        candidates.push_back(std::string(runfiles) + "/" + relative);
    candidates.push_back("../" + relative);  // the working directory of bazel run: <runfiles>/_main
    for (const std::string& c : candidates)
        if (access(c.c_str(), R_OK) == 0) {
            char resolved[4096];
            return realpath(c.c_str(), resolved) ? std::string(resolved) : c;
        }
    return "";
#endif
}


std::string ComponentType(uint32_t type) {
    switch (type) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR: return "fp16";
        case VK_COMPONENT_TYPE_FLOAT32_KHR: return "fp32";
        case VK_COMPONENT_TYPE_FLOAT64_KHR: return "fp64";
        case VK_COMPONENT_TYPE_SINT8_KHR: return "s8";
        case VK_COMPONENT_TYPE_SINT16_KHR: return "s16";
        case VK_COMPONENT_TYPE_SINT32_KHR: return "s32";
        case VK_COMPONENT_TYPE_UINT8_KHR: return "u8";
        case VK_COMPONENT_TYPE_UINT16_KHR: return "u16";
        case VK_COMPONENT_TYPE_UINT32_KHR: return "u32";
        case VK_COMPONENT_TYPE_BFLOAT16_KHR: return "bf16";
        case VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT: return "e4m3";
        case VK_COMPONENT_TYPE_FLOAT8_E5M2_EXT: return "e5m2";
        default: return std::to_string(type);
    }
}

std::string Scope(uint32_t scope) {
    switch (scope) {
        case VK_SCOPE_DEVICE_KHR: return "device";
        case VK_SCOPE_WORKGROUP_KHR: return "workgroup";
        case VK_SCOPE_SUBGROUP_KHR: return "subgroup";
        case VK_SCOPE_QUEUE_FAMILY_KHR: return "queue_family";
        default: return std::to_string(scope);
    }
}

// The physical device's capabilities (VulkanCapabilities), with the structures its API version and extensions allow.
VulkanCapabilities Query(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical,
                         const std::vector<VkExtensionProperties>& extensions) {
    VulkanCapabilities caps;
    auto get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
    auto get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance, "vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties subgroup = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties subgroup_size = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceShaderIntegerDotProductProperties dot = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES};
    VkPhysicalDeviceShaderSMBuiltinsPropertiesNV sm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV};
    VkPhysicalDeviceShaderCoreProperties2AMD amd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_2_AMD};
    VkPhysicalDeviceProperties basic;
    ((PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties"))(physical, &basic);
    const bool v13 = basic.apiVersion >= VK_API_VERSION_1_3;
    void** next = &properties.pNext;
    auto chain = [&](void* s, void** s_next) {
        *next = s;
        next = s_next;
    };
    chain(&driver, &driver.pNext);
    chain(&subgroup, &subgroup.pNext);
    if (v13) {
        chain(&subgroup_size, &subgroup_size.pNext);
        chain(&dot, &dot.pNext);
    }
    if (Has(extensions, VK_NV_SHADER_SM_BUILTINS_EXTENSION_NAME))
        chain(&sm, &sm.pNext);
    if (Has(extensions, VK_AMD_SHADER_CORE_PROPERTIES_2_EXTENSION_NAME))
        chain(&amd, &amd.pNext);
    get_properties2(physical, &properties);
    caps.device_name = basic.deviceName;
    caps.vendor_id = basic.vendorID;
    caps.device_id = basic.deviceID;
    caps.driver_version = basic.driverVersion;
    caps.api_version = basic.apiVersion;
    caps.driver_id = driver.driverID;
    caps.driver_name = driver.driverName;
    caps.driver_info = driver.driverInfo;
    caps.subgroup_size = subgroup.subgroupSize;
    caps.subgroup_min = v13 ? subgroup_size.minSubgroupSize : subgroup.subgroupSize;
    caps.subgroup_max = v13 ? subgroup_size.maxSubgroupSize : subgroup.subgroupSize;
    caps.required_subgroup_compute = v13 && (subgroup_size.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
    caps.dot4x8_accelerated = v13 && dot.integerDotProduct4x8BitPackedSignedAccelerated;
    caps.max_shared_bytes = basic.limits.maxComputeSharedMemorySize;
    caps.max_invocations = basic.limits.maxComputeWorkGroupInvocations;
    caps.sm_count = sm.shaderSMCount;
    caps.warps_per_sm = sm.shaderWarpsPerSM;
    caps.compute_units = amd.activeComputeUnitCount;

    VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceShaderFloat8FeaturesEXT float8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomic_float = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    next = &features.pNext;
    chain(&f11, &f11.pNext);
    chain(&f12, &f12.pNext);
    if (Has(extensions, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME))
        chain(&float8, &float8.pNext);
    if (Has(extensions, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME))
        chain(&atomic_float, &atomic_float.pNext);
    get_features2(physical, &features);
    caps.float16 = f12.shaderFloat16;
    caps.int8 = f12.shaderInt8;
    caps.storage16 = f11.storageBuffer16BitAccess;
    caps.int64_atomics = f12.shaderBufferInt64Atomics;
    caps.shared_float_atomics = atomic_float.shaderSharedFloat32AtomicAdd;
    caps.float8 = float8.shaderFloat8;
    caps.float8_cooperative_matrix = float8.shaderFloat8CooperativeMatrix;

    caps.cooperative_matrix = Has(extensions, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    auto matrix_properties = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)gipa(
        instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    uint32_t count = 0;
    if (caps.cooperative_matrix && matrix_properties && matrix_properties(physical, &count, nullptr) == VK_SUCCESS) {
        std::vector<VkCooperativeMatrixPropertiesKHR> formats(count, {VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
        if (matrix_properties(physical, &count, formats.data()) == VK_SUCCESS)
            for (const auto& f : formats)
                caps.matrices.push_back({f.MSize, f.NSize, f.KSize, ComponentType(f.AType), ComponentType(f.BType),
                                         ComponentType(f.CType), ComponentType(f.ResultType), Scope(f.scope),
                                         f.saturatingAccumulation != VK_FALSE});
    }
    return caps;
}

}  // namespace

VulkanCapabilities QueryCapabilities(void* get_instance_proc, void* instance, void* physical_device) {
    auto gipa = (PFN_vkGetInstanceProcAddr)get_instance_proc;
    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)gipa((VkInstance)instance,
                                                                    "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> extensions;
    uint32_t count = 0;
    if (enumerate && enumerate((VkPhysicalDevice)physical_device, nullptr, &count, nullptr) == VK_SUCCESS) {
        extensions.resize(count);
        if (enumerate((VkPhysicalDevice)physical_device, nullptr, &count, extensions.data()) != VK_SUCCESS)
            count = 0;
        extensions.resize(count);
    }
    return Query(gipa, (VkInstance)instance, (VkPhysicalDevice)physical_device, extensions);
}

std::string VulkanCapabilities::DriverVersion() const {
    char text[64];
    const uint32_t v = driver_version;
    if (vendor_id == 0x10DE)
        std::snprintf(text, sizeof(text), "%u.%02u.%02u", v >> 22, (v >> 14) & 0xff, (v >> 6) & 0xff);
    else if (vendor_id == 0x8086 && driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS)
        std::snprintf(text, sizeof(text), "%u.%u", v >> 14, v & 0x3fff);
    else
        std::snprintf(text, sizeof(text), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
                      VK_API_VERSION_PATCH(v));
    return text;
}

bool VulkanDevice::Create(bool window, const std::vector<std::string>& extra_instance_extensions,
                          const std::vector<std::string>& extra_device_extensions,
                          const nri::VKBindingOffsets& binding_offsets, std::string& error,
                          const std::function<std::vector<std::string>(void*, void*)>& device_extensions_for) {
    loader_path = LoaderPath();
#ifndef _WIN32
    if (loader_path.empty())
        spdlog::warn("the Vulkan loader built from source is not in the runfiles; using the system's {}", SYSTEM_LOADER);
#endif
    const std::string loader_name = loader_path.empty() ? SYSTEM_LOADER : loader_path;
    void* loader = OpenLibrary(loader_name);
    if (!loader) {
        error = "the Vulkan loader could not be loaded (" + loader_name + ")";
        return false;
    }
    auto gipa = (PFN_vkGetInstanceProcAddr)Symbol(loader, "vkGetInstanceProcAddr");
    get_instance_proc = (void*)gipa;
    auto enumerate_instance_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties");
    auto create_instance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");

    uint32_t n = 0;
    enumerate_instance_extensions(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available_instance(n);
    enumerate_instance_extensions(nullptr, &n, available_instance.data());

    std::vector<std::string> wanted_instance = extra_instance_extensions;
    if (window) {
        AddUnique(wanted_instance, VK_KHR_SURFACE_EXTENSION_NAME);
        AddUnique(wanted_instance, "VK_KHR_xlib_surface");  // the ones the system has stay
        AddUnique(wanted_instance, "VK_KHR_wayland_surface");
        AddUnique(wanted_instance, "VK_KHR_win32_surface");
        AddUnique(wanted_instance, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }
    for (const std::string& e : wanted_instance)
        if (Has(available_instance, e))
            AddUnique(instance_extensions, e);
    std::vector<const char*> instance_names;
    for (const std::string& e : instance_extensions)
        instance_names.push_back(e.c_str());

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "neural-rendering";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = (uint32_t)instance_names.size();
    instance_info.ppEnabledExtensionNames = instance_names.data();
    VkInstance vk_instance = VK_NULL_HANDLE;
    if (create_instance(&instance_info, nullptr, &vk_instance) != VK_SUCCESS) {
        error = "vkCreateInstance failed";
        return false;
    }
    instance = vk_instance;

    auto enumerate_devices = (PFN_vkEnumeratePhysicalDevices)gipa(vk_instance, "vkEnumeratePhysicalDevices");
    auto get_properties = (PFN_vkGetPhysicalDeviceProperties)gipa(vk_instance, "vkGetPhysicalDeviceProperties");
    auto get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(vk_instance, "vkGetPhysicalDeviceFeatures2");
    auto enumerate_device_extensions =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(vk_instance, "vkEnumerateDeviceExtensionProperties");
    auto create_device = (PFN_vkCreateDevice)gipa(vk_instance, "vkCreateDevice");
    auto get_queue_families =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(vk_instance, "vkGetPhysicalDeviceQueueFamilyProperties");

    uint32_t device_count = 0;
    enumerate_devices(vk_instance, &device_count, nullptr);
    std::vector<VkPhysicalDevice> devices(device_count);
    enumerate_devices(vk_instance, &device_count, devices.data());
    // The first discrete GPU, else the first integrated one (handhelds such as the Steam Deck). NR_VULKAN_DEVICE
    // (an index into vkEnumeratePhysicalDevices) picks another.
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    const char* forced_device = std::getenv("NR_VULKAN_DEVICE");
    if (forced_device && *forced_device) {
        const uint32_t index = (uint32_t)std::strtoul(forced_device, nullptr, 10);
        if (index >= device_count) {
            error = "NR_VULKAN_DEVICE " + std::string(forced_device) + ": there are " + std::to_string(device_count) +
                    " devices";
            return false;
        }
        chosen = devices[index];
    }
    for (VkPhysicalDeviceType type : {VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU})
        for (VkPhysicalDevice d : devices) {
            VkPhysicalDeviceProperties p;
            get_properties(d, &p);
            if (!chosen && p.deviceType == type)
                chosen = d;
        }
    if (!chosen) {
        error = "no discrete or integrated GPU";
        return false;
    }
    physical_device = chosen;

    uint32_t family_count = 0;
    get_queue_families(chosen, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    get_queue_families(chosen, &family_count, families.data());
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            queue_family = i;
            break;
        }
    if (queue_family == UINT32_MAX) {
        error = "no graphics and compute queue";
        return false;
    }

    enumerate_device_extensions(chosen, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available_device(n);
    enumerate_device_extensions(chosen, nullptr, &n, available_device.data());
    caps = Query(gipa, vk_instance, chosen, available_device);
    std::vector<std::string> wanted_device = extra_device_extensions;
    const bool request_float8 = std::find(extra_device_extensions.begin(), extra_device_extensions.end(),
                                          VK_EXT_SHADER_FLOAT8_EXTENSION_NAME) != extra_device_extensions.end();
    if (request_float8 && !Has(available_device, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) {
        error = "VK_EXT_shader_float8 was asked for, and the device does not have it";
        return false;
    }
    if (device_extensions_for)
        for (const std::string& e : device_extensions_for(vk_instance, chosen))
            AddUnique(wanted_device, e);
    for (const char* e : {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
                          VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
                          VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                          VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                          // NRI's root descriptors: on an Arc B580 (driver 101.6252, Vulkan 1.4) vkCmdPushDescriptorSet
                          // was null without it, and the viewer crashed calling it
                          VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME})
        AddUnique(wanted_device, e);
    if (window)
        AddUnique(wanted_device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    video_decode_queue_family = UINT32_MAX;
    if (video_decode) {
        for (uint32_t i = 0; i < family_count; i++)
            if ((families[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && i != queue_family) {
                video_decode_queue_family = i;
                break;
            }
        if (video_decode_queue_family != UINT32_MAX)
            for (const char* e : {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
                                  VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
                                  VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
                                  VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME})
                AddUnique(wanted_device, e);
    }
    for (const std::string& e : wanted_device) {
        if (Has(available_device, e))
            AddUnique(device_extensions, e);
        else
            spdlog::debug("device extension {} is not available", e);
    }
    // A video decode queue needs the queue and decode extensions; without them the device has no such queue.
    if (video_decode_queue_family != UINT32_MAX &&
        !(std::find(device_extensions.begin(), device_extensions.end(), VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) != device_extensions.end() &&
          std::find(device_extensions.begin(), device_extensions.end(), VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) != device_extensions.end())) {
        spdlog::debug("{} lacks VK_KHR_video_queue or VK_KHR_video_decode_queue: no video decode queue", caps.device_name);
        video_decode_queue_family = UINT32_MAX;
    }
    // Cooperative matrices are always asked for; a GPU without them is worth a line in the log.
    if (!Has(available_device, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME))
        spdlog::warn("{} has no VK_KHR_cooperative_matrix", caps.device_name);
    std::vector<const char*> device_names;
    for (const std::string& e : device_extensions)
        device_names.push_back(e.c_str());
    auto enabled = [&](const char* name) {
        return std::find(device_extensions.begin(), device_extensions.end(), name) != device_extensions.end();
    };

    // Every feature the device reports, chained for the enabled extensions; robust buffer access off (it costs
    // time and the kernels stay in bounds).
    VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features features11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features features12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features features14 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT float8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomic_float = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamic_state = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceOpticalFlowFeaturesNV optical_flow = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV};
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR video_maintenance1 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR};
    VkPhysicalDeviceVideoMaintenance2FeaturesKHR video_maintenance2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    void** next = &features2.pNext;
    auto chain = [&](void* s, void** s_next) {
        *next = s;
        next = s_next;
    };
    chain(&features11, &features11.pNext);
    chain(&features12, &features12.pNext);
    // The core feature structures of the device's API version only (Turing to Blackwell report 1.4 on current
    // drivers; older drivers may not).
    if (caps.api_version >= VK_API_VERSION_1_3)
        chain(&features13, &features13.pNext);
    if (caps.api_version >= VK_API_VERSION_1_4)
        chain(&features14, &features14.pNext);
    if (enabled(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME))
        chain(&cooperative, &cooperative.pNext);
    if (request_float8)
        chain(&float8, &float8.pNext);
    if (enabled(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME))
        chain(&atomic_float, &atomic_float.pNext);
    if (enabled(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME))
        chain(&dynamic_state, &dynamic_state.pNext);
    if (enabled(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME))
        chain(&acceleration, &acceleration.pNext);
    if (enabled(VK_KHR_RAY_QUERY_EXTENSION_NAME))
        chain(&ray_query, &ray_query.pNext);
    if (enabled(VK_NV_OPTICAL_FLOW_EXTENSION_NAME))  // NGX frame generation on GPUs with an optical flow unit
        chain(&optical_flow, &optical_flow.pNext);
    if (enabled(VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME))
        chain(&video_maintenance1, &video_maintenance1.pNext);
    if (enabled(VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME))
        chain(&video_maintenance2, &video_maintenance2.pNext);
    if (enabled(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME))  // FSR 3.1 frame generation
        chain(&derivatives, &derivatives.pNext);
    get_features2(chosen, &features2);
    if (request_float8) {
        auto get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(vk_instance, "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceSubgroupProperties subgroup = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &subgroup;
        if (!get_properties2) {
            error = "could not query FP8 subgroup size before device creation";
            return false;
        }
        get_properties2(chosen, &properties);
        if (subgroup.subgroupSize != 32) {
            error = "VK_EXT_shader_float8 is used here with 32-lane subgroups only";
            return false;
        }
        if (!float8.shaderFloat8 || !float8.shaderFloat8CooperativeMatrix) {
            error = "VK_EXT_shader_float8 was asked for without shaderFloat8 and shaderFloat8CooperativeMatrix";
            return false;
        }
        auto matrix_properties = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
            gipa(vk_instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
        uint32_t count = 0;
        if (!matrix_properties || matrix_properties(chosen, &count, nullptr) != VK_SUCCESS) {
            error = "could not query FP8 cooperative matrix formats before device creation";
            return false;
        }
        std::vector<VkCooperativeMatrixPropertiesKHR> formats(count);
        for (auto& format : formats)
            format.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
        if (matrix_properties(chosen, &count, formats.data()) != VK_SUCCESS ||
            !std::any_of(formats.begin(), formats.end(), [](const auto& f) {
                return f.MSize == 16 && f.NSize == 16 && f.KSize == 32 &&
                       f.AType == VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT && f.BType == VK_COMPONENT_TYPE_FLOAT8_E4M3_EXT &&
                       f.CType == VK_COMPONENT_TYPE_FLOAT32_KHR && f.ResultType == VK_COMPONENT_TYPE_FLOAT32_KHR &&
                       f.scope == VK_SCOPE_SUBGROUP_KHR && !f.saturatingAccumulation;
            })) {
            error = "VK_EXT_shader_float8 is used here with subgroup E4M3 16x16x32 products with FP32 accumulation only";
            return false;
        }
    }
    features2.features.robustBufferAccess = VK_FALSE;
    features14.pipelineRobustness = VK_FALSE;
    acceleration.accelerationStructureCaptureReplay = VK_FALSE;
    acceleration.accelerationStructureHostCommands = VK_FALSE;

    const float priority = 1.0f;
    const float priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queue_infos[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO},
                                              {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    queue_infos[0].queueFamilyIndex = queue_family;
    queue_infos[0].queueCount = 1;
    queue_infos[0].pQueuePriorities = &priority;
    uint32_t queue_info_count = 1;
    video_decode_queue_num = 0;
    if (video_decode_queue_family != UINT32_MAX) {
        video_decode_queue_num = std::min<uint32_t>(families[video_decode_queue_family].queueCount, 2);
        queue_infos[1].queueFamilyIndex = video_decode_queue_family;
        queue_infos[1].queueCount = video_decode_queue_num;
        queue_infos[1].pQueuePriorities = priorities;
        queue_info_count = 2;
    }
    VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features2;
    device_info.queueCreateInfoCount = queue_info_count;
    device_info.pQueueCreateInfos = queue_infos;
    device_info.enabledExtensionCount = (uint32_t)device_names.size();
    device_info.ppEnabledExtensionNames = device_names.data();
    VkDevice vk_device = VK_NULL_HANDLE;
    if (create_device(chosen, &device_info, nullptr, &vk_device) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        return false;
    }
    device = vk_device;

    nri::QueueFamilyVKDesc families_vk[2] = {};
    families_vk[0].queueType = nri::QueueType::GRAPHICS;
    families_vk[0].queueNum = 1;
    families_vk[0].familyIndex = queue_family;
    if (video_decode_queue_family != UINT32_MAX) {
        families_vk[1].queueType = nri::QueueType::VIDEO_DECODE;
        families_vk[1].queueNum = video_decode_queue_num;
        families_vk[1].familyIndex = video_decode_queue_family;
    }
    nri::DeviceCreationVKDesc desc = {};
    desc.callbackInterface = NriLogCallback();
    desc.libraryPath = loader_path.empty() ? nullptr : loader_path.c_str();  // NRI loads the same loader
    desc.vkBindingOffsets = binding_offsets;
    desc.vkExtensions.instanceExtensions = instance_names.data();
    desc.vkExtensions.instanceExtensionNum = (uint32_t)instance_names.size();
    desc.vkExtensions.deviceExtensions = device_names.data();
    desc.vkExtensions.deviceExtensionNum = (uint32_t)device_names.size();
    desc.vkInstance = (VKHandle)vk_instance;
    desc.vkDevice = (VKHandle)vk_device;
    desc.vkPhysicalDevice = (VKHandle)chosen;
    desc.queueFamilies = families_vk;
    desc.queueFamilyNum = video_decode_queue_family != UINT32_MAX ? 2 : 1;
    desc.minorVersion = 4;
    desc.enableNRIValidation = std::getenv("NR_NRI_VALIDATION") != nullptr;  // debugging: NRI's own checks
    if (nri::nriCreateDeviceFromVKDevice(desc, nri) != nri::Result::SUCCESS) {
        error = "NRI could not wrap the Vulkan device";
        return false;
    }
    return true;
}

void VulkanDevice::Destroy() {
    if (nri)
        nri::nriDestroyDevice(nri);
    nri = nullptr;
    auto gipa = (PFN_vkGetInstanceProcAddr)get_instance_proc;
    if (device) {
        auto destroy = (PFN_vkDestroyDevice)gipa((VkInstance)instance, "vkDestroyDevice");
        destroy((VkDevice)device, nullptr);
    }
    if (instance) {
        auto destroy = (PFN_vkDestroyInstance)gipa((VkInstance)instance, "vkDestroyInstance");
        destroy((VkInstance)instance, nullptr);
    }
    device = instance = physical_device = nullptr;
}

}  // namespace nr
