#include "record/vulkan_ring.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace record {
namespace {

// The coding's flags (record_pack.slang REC_*).
constexpr uint32_t REC_DEPTH = 1, REC_REFERENCE = 2, REC_SRGB = 4;
// Descriptor sets (the textures' bindings), reused in a ring long enough that the GPU has finished with a set before
// it comes round (TABLES / 8 real frames of 8 textures).
constexpr uint32_t TABLES = 1024;
constexpr uint32_t BINDINGS = 5;  // src, src2, record_slot, record_status, record_counter

std::vector<char> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool IsDepth(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM ||
           f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32;
}

bool HasStencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

// Layouts a sampled image may be read in; others go to GENERAL for the pack and back.
bool Readable(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_GENERAL || l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
}

}  // namespace

bool DescribeVulkan(VkFormat format, uint32_t width, uint32_t height, Channels& out, std::string& error) {
    out = {};
    out.width = width;
    out.height = height;
    out.vk_format = (uint32_t)format;
    switch (format) {
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: out.channels = 4, out.kinds = 10 | 10 << 8 | 10 << 16 | 2 << 24; break;
    // Each 11- or 10-bit float is exactly an FP16 value: coded as FP16 and packed again by the reader.
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: out.channels = 3, out.kinds = F16 * 0x010101u; break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: out.channels = 4, out.kinds = 0x08080808; break;
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB:
        out.srgb = true, out.channels = 4, out.kinds = 0x08080808; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: out.channels = 4, out.kinds = F16 * 0x01010101u; break;
    case VK_FORMAT_R16G16B16A16_UNORM: out.channels = 4, out.kinds = 0x10101010; break;
    case VK_FORMAT_R16G16_SFLOAT: out.channels = 2, out.kinds = F16 | F16 << 8; break;
    case VK_FORMAT_R16G16_UNORM: out.channels = 2, out.kinds = 0x1010; break;
    case VK_FORMAT_R32G32_SFLOAT: out.channels = 2, out.kinds = F32 | F32 << 8; break;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: out.depth = true, out.channels = 1, out.kinds = F32; break;
    case VK_FORMAT_D32_SFLOAT: out.depth = true, out.channels = 1, out.kinds = F32; break;
    case VK_FORMAT_R32_SFLOAT: out.channels = 1, out.kinds = F32; break;
    case VK_FORMAT_R16_SFLOAT: out.channels = 1, out.kinds = F16; break;
    case VK_FORMAT_R16_UNORM: out.channels = 1, out.kinds = 16; break;
    case VK_FORMAT_D16_UNORM: out.vk_format = VK_FORMAT_R16_UNORM, out.depth = true, out.channels = 1, out.kinds = 16;
        break;
    case VK_FORMAT_R8_UNORM: out.channels = 1, out.kinds = 8; break;
    default:
        error = "no lossless coding for VkFormat " + std::to_string((int)format);
        return false;
    }
    if (!width || !height || width > 16384) {
        error = "not a 2D image up to 16384 wide";
        return false;
    }
    return true;
}

struct VulkanRing::Impl {
    VulkanDevice dev;
    // Device commands, from the game's vkGetDeviceProcAddr.
#define RING_FUNCTIONS(X)                                                                                              \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)    \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline)              \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)         \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)          \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges) X(vkCreateImageView)       \
    X(vkDestroyImageView) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCreateEvent)         \
    X(vkDestroyEvent) X(vkResetEvent) X(vkGetEventStatus) X(vkCmdPipelineBarrier) X(vkCmdBindPipeline)                 \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp)    \
    X(vkCmdSetEvent) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)     \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdCopyBuffer) X(vkQueueSubmit) X(vkCreateFence)                 \
    X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)
#define RING_DECLARE(name) PFN_##name name = nullptr;
    RING_FUNCTIONS(RING_DECLARE)
    VkPhysicalDeviceMemoryProperties memory = {};
    double timestamp_ns = 0;  // 0: no timestamps
    VkDeviceSize max_range = 0;
    VkShaderModule modules[3] = {};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline init = VK_NULL_HANDLE, pack = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets;
    uint32_t next_set = 0;
    struct Memory {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        VkDeviceSize bytes = 0;
        bool coherent = true;
    };
    Memory status, counter;  // status: host memory, per slot used words and serial; counter: device memory
    Memory slots[MAX_SLOTS];      // where the pack writes: host memory, or with a copy queue device memory
    Memory readback[MAX_SLOTS];   // with a copy queue: the slots' copies in host memory
    unsigned slot_count = 0;
    // The recorder's queue (copy_family), its command buffer and fence; the copier thread's alone.
    VkQueue copy_queue = VK_NULL_HANDLE;
    VkCommandPool copy_pool = VK_NULL_HANDLE;
    VkCommandBuffer copy_cb = VK_NULL_HANDLE;
    VkFence copy_fence = VK_NULL_HANDLE;
    std::vector<uint32_t> families;  // every queue family: the device-memory slots are shared by the game's and ours
    std::vector<VkImageView> views[MAX_SLOTS];  // the slot's last pack's; destroyed when the slot is packed again
    VkQueryPool queries = VK_NULL_HANDLE;
    VkEvent events[MAX_SLOTS] = {};
    uint32_t serial = 0;
    std::array<std::atomic<uint32_t>, MAX_SLOTS> expected = {};

    // A buffer in host memory (HOST_VISIBLE, preferring HOST_CACHED and not DEVICE_LOCAL), mapped, or in device
    // memory. Cached host memory costs the GPU's writes more than write-combined memory (RTX 5090, 4K PQ10 with
    // HUD-less color, D32S8, RG16F: 2.03 against 1.32 ms per pack) but the workers read it 5 times as fast (19 against
    // 98 ms of thread time per frame), which keeps the ring free.
    bool MakeBuffer(Memory& m, VkDeviceSize bytes, bool host, std::string& error,
                    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (copy_queue && families.size() > 1 && (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = (uint32_t)families.size();
            info.pQueueFamilyIndices = families.data();
        }
        if (vkCreateBuffer(dev.device, &info, nullptr, &m.buffer) != VK_SUCCESS) {
            error = "a buffer of " + std::to_string(bytes) + " bytes";
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev.device, m.buffer, &req);
        int best = -1, best_score = -1;
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
            if (!(req.memoryTypeBits & (1u << i)))
                continue;
            const VkMemoryPropertyFlags f = memory.memoryTypes[i].propertyFlags;
            int score = 0;
            if (host) {
                if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                    continue;
                score = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? 4 : 0) +
                        (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? 0 : 2) +
                        (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? 1 : 0);
            } else {
                score = f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? 1 : 0;
            }
            if (score > best_score)
                best = (int)i, best_score = score;
        }
        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = (uint32_t)best;
        if (best < 0 || vkAllocateMemory(dev.device, &alloc, nullptr, &m.memory) != VK_SUCCESS ||
            vkBindBufferMemory(dev.device, m.buffer, m.memory, 0) != VK_SUCCESS) {
            error = std::string(host ? "host" : "device") + " memory of " + std::to_string(bytes >> 20) + " MiB";
            Free(m);
            return false;
        }
        m.bytes = bytes;
        m.coherent = (memory.memoryTypes[best].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        void* data = nullptr;
        if (host && vkMapMemory(dev.device, m.memory, 0, VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS) {
            error = "mapping " + std::to_string(bytes >> 20) + " MiB of host memory";
            Free(m);
            return false;
        }
        m.mapped = (uint8_t*)data;
        return true;
    }
    void Free(Memory& m) {
        if (m.mapped)
            vkUnmapMemory(dev.device, m.memory);
        if (m.buffer)
            vkDestroyBuffer(dev.device, m.buffer, nullptr);
        if (m.memory)
            vkFreeMemory(dev.device, m.memory, nullptr);
        m = {};
    }
    void Invalidate(const Memory& m) {
        if (m.coherent)
            return;
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, m.memory, 0, VK_WHOLE_SIZE};
        vkInvalidateMappedMemoryRanges(dev.device, 1, &range);
    }
    VkImageView View(const VulkanTexture& t, std::string& error) {
        VkImageViewUsageCreateInfo usage = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &usage};
        info.image = t.image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = t.format;
        info.subresourceRange = {(VkImageAspectFlags)(IsDepth(t.format) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                         : VK_IMAGE_ASPECT_COLOR_BIT),
                                 t.range.baseMipLevel, 1, t.range.baseArrayLayer, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(dev.device, &info, nullptr, &view) != VK_SUCCESS) {
            error = "an image view of VkFormat " + std::to_string((int)t.format);
            return VK_NULL_HANDLE;
        }
        return view;
    }
};

std::shared_ptr<VulkanRing> VulkanRing::Make(const VulkanDevice& device, const std::string& kernel_dir,
                                             std::string& error) {
    auto r = std::shared_ptr<VulkanRing>(new VulkanRing());
    r->impl_ = std::make_unique<Impl>();
    Impl& m = *r->impl_;
    m.dev = device;
    if (!m.dev.gipa) {
        HMODULE loader = LoadLibraryA("vulkan-1.dll");
        m.dev.gipa = loader ? (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr") : nullptr;
    }
    if (!m.dev.gdpa && m.dev.gipa)
        m.dev.gdpa = (PFN_vkGetDeviceProcAddr)m.dev.gipa(m.dev.instance, "vkGetDeviceProcAddr");
    if (!m.dev.device || !m.dev.physical || !m.dev.gipa || !m.dev.gdpa) {
        error = "no Vulkan device or entry points";
        return nullptr;
    }
#define RING_LOAD(name)                                                                                                \
    if (!(m.name = (PFN_##name)m.dev.gdpa(m.dev.device, #name))) {                                                     \
        error = "no " #name;                                                                                           \
        return nullptr;                                                                                                \
    }
    RING_FUNCTIONS(RING_LOAD)
    auto memory_properties = (PFN_vkGetPhysicalDeviceMemoryProperties)m.dev.gipa(
        m.dev.instance, "vkGetPhysicalDeviceMemoryProperties");
    auto properties = (PFN_vkGetPhysicalDeviceProperties)m.dev.gipa(m.dev.instance, "vkGetPhysicalDeviceProperties");
    if (!memory_properties || !properties) {
        error = "no vkGetPhysicalDeviceMemoryProperties or vkGetPhysicalDeviceProperties";
        return nullptr;
    }
    memory_properties(m.dev.physical, &m.memory);
    VkPhysicalDeviceProperties props;
    properties(m.dev.physical, &props);
    m.max_range = props.limits.maxStorageBufferRange;
    if (props.limits.timestampComputeAndGraphics)
        m.timestamp_ns = props.limits.timestampPeriod;

    const char* entries[3] = {"record_init", "record_pack", "record_done"};
    for (int i = 0; i < 3; i++) {
        const std::string path = kernel_dir + "\\record_pack.slang." + entries[i] + ".spv";
        const std::vector<char> code = ReadFile(path);
        VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size();
        info.pCode = (const uint32_t*)code.data();
        if (code.empty() || code.size() % 4 ||
            m.vkCreateShaderModule(m.dev.device, &info, nullptr, &m.modules[i]) != VK_SUCCESS) {
            error = code.empty() ? "no kernel " + path : "the shader module of " + std::string(entries[i]);
            return nullptr;
        }
    }
    VkDescriptorSetLayoutBinding bindings[BINDINGS] = {};
    for (uint32_t i = 0; i < BINDINGS; i++)
        bindings[i] = {i, i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = BINDINGS;
    set_info.pBindings = bindings;
    const VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (m.vkCreateDescriptorSetLayout(m.dev.device, &set_info, nullptr, &m.set_layout) != VK_SUCCESS ||
        (layout_info.pSetLayouts = &m.set_layout,
         m.vkCreatePipelineLayout(m.dev.device, &layout_info, nullptr, &m.pipeline_layout) != VK_SUCCESS)) {
        error = "the pipeline layout";
        return nullptr;
    }
    VkPipeline* pipelines[3] = {&m.init, &m.pack, &m.done};
    for (int i = 0; i < 3; i++) {
        VkComputePipelineCreateInfo info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                      m.modules[i], "main", nullptr};  // slangc names each module's entry main
        info.layout = m.pipeline_layout;
        if (m.vkCreateComputePipelines(m.dev.device, VK_NULL_HANDLE, 1, &info, nullptr, pipelines[i]) != VK_SUCCESS) {
            error = "the pipeline of " + std::string(entries[i]);
            return nullptr;
        }
    }
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * TABLES},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * TABLES}};
    VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = TABLES;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    if (m.vkCreateDescriptorPool(m.dev.device, &pool_info, nullptr, &m.pool) != VK_SUCCESS) {
        error = "the descriptor pool";
        return nullptr;
    }
    m.sets.resize(TABLES);
    const std::vector<VkDescriptorSetLayout> layouts(TABLES, m.set_layout);
    VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = m.pool;
    alloc.descriptorSetCount = TABLES;
    alloc.pSetLayouts = layouts.data();
    if (m.vkAllocateDescriptorSets(m.dev.device, &alloc, m.sets.data()) != VK_SUCCESS) {
        error = "the descriptor sets";
        return nullptr;
    }
    if (!m.MakeBuffer(m.status, 8 * MAX_SLOTS, true, error) || !m.MakeBuffer(m.counter, 4 * MAX_SLOTS, false, error))
        return nullptr;
    std::memset(m.status.mapped, 0, 8 * MAX_SLOTS);
    VkQueryPoolCreateInfo query_info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = 2 * MAX_SLOTS;
    if (m.timestamp_ns && m.vkCreateQueryPool(m.dev.device, &query_info, nullptr, &m.queries) != VK_SUCCESS)
        m.queries = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < MAX_SLOTS; i++) {
        VkEventCreateInfo event_info = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        if (m.vkCreateEvent(m.dev.device, &event_info, nullptr, &m.events[i]) != VK_SUCCESS) {
            error = "the events";
            return nullptr;
        }
    }
    // The recorder's queue, where a provider made one; else the slots stay in host memory.
    if (m.dev.copy_family != UINT32_MAX) {
        auto family_properties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)m.dev.gipa(
            m.dev.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t count = 0;
        if (family_properties)
            family_properties(m.dev.physical, &count, nullptr);
        for (uint32_t i = 0; i < count; i++)
            m.families.push_back(i);
        m.vkGetDeviceQueue(m.dev.device, m.dev.copy_family, m.dev.copy_index, &m.copy_queue);
        VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = m.dev.copy_family;
        VkCommandBufferAllocateInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb.commandBufferCount = 1;
        VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!m.copy_queue || m.vkCreateCommandPool(m.dev.device, &pool, nullptr, &m.copy_pool) != VK_SUCCESS ||
            (cb.commandPool = m.copy_pool,
             m.vkAllocateCommandBuffers(m.dev.device, &cb, &m.copy_cb) != VK_SUCCESS) ||
            m.vkCreateFence(m.dev.device, &fence, nullptr, &m.copy_fence) != VK_SUCCESS) {
            if (m.copy_pool)
                m.vkDestroyCommandPool(m.dev.device, m.copy_pool, nullptr);
            m.copy_pool = VK_NULL_HANDLE;
            m.copy_queue = VK_NULL_HANDLE;  // the direct path
        }
    }
    return r;
}

VulkanRing::~VulkanRing() {
    if (!impl_ || !impl_->dev.device)
        return;
    Impl& m = *impl_;
    const VkDevice d = m.dev.device;
    for (uint32_t i = 0; i < MAX_SLOTS; i++) {
        for (VkImageView v : m.views[i])
            if (m.vkDestroyImageView)
                m.vkDestroyImageView(d, v, nullptr);
        if (m.events[i])
            m.vkDestroyEvent(d, m.events[i], nullptr);
        if (m.vkFreeMemory) {
            m.Free(m.slots[i]);
            m.Free(m.readback[i]);
        }
    }
    if (m.vkFreeMemory) {
        m.Free(m.status);
        m.Free(m.counter);
    }
    if (m.queries)
        m.vkDestroyQueryPool(d, m.queries, nullptr);
    if (m.copy_fence)
        m.vkDestroyFence(d, m.copy_fence, nullptr);
    if (m.copy_pool)
        m.vkDestroyCommandPool(d, m.copy_pool, nullptr);
    if (m.pool)
        m.vkDestroyDescriptorPool(d, m.pool, nullptr);
    for (VkPipeline p : {m.init, m.pack, m.done})
        if (p)
            m.vkDestroyPipeline(d, p, nullptr);
    if (m.pipeline_layout)
        m.vkDestroyPipelineLayout(d, m.pipeline_layout, nullptr);
    if (m.set_layout)
        m.vkDestroyDescriptorSetLayout(d, m.set_layout, nullptr);
    for (VkShaderModule s : m.modules)
        if (s)
            m.vkDestroyShaderModule(d, s, nullptr);
}

VkDevice VulkanRing::device() const { return impl_->dev.device; }
bool VulkanRing::copies() const { return impl_->copy_queue != VK_NULL_HANDLE; }

bool VulkanRing::Describe(const void* texture, Channels& out, std::string& error) {
    const VulkanTexture& t = *(const VulkanTexture*)texture;
    return t.image && DescribeVulkan(t.format, t.width, t.height, out, error);
}

// The slots of the last recording are reused when they are as many and large enough; a recording starts after the
// last one's writer finished, so its packs have run (or were lost two seconds before).
bool VulkanRing::Create(unsigned slots, uint64_t bytes, std::string& error) {
    Impl& m = *impl_;
    if (bytes > m.max_range) {
        error = "a slot of " + std::to_string(bytes >> 20) + " MiB is above the device's storage buffer range";
        return false;
    }
    for (uint32_t i = 0; i < MAX_SLOTS; i++)
        m.expected[i] = 0;
    if (m.slot_count == slots && m.slots[0].bytes >= bytes)
        return true;
    auto free_all = [&] {
        for (uint32_t i = 0; i < MAX_SLOTS; i++) {
            m.Free(m.slots[i]);
            m.Free(m.readback[i]);
        }
    };
    free_all();
    m.slot_count = 0;
    const bool copy = m.copy_queue != VK_NULL_HANDLE;
    for (unsigned i = 0; i < slots; i++)
        if (copy ? !m.MakeBuffer(m.slots[i], bytes, false, error,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
                       !m.MakeBuffer(m.readback[i], bytes, true, error, VK_BUFFER_USAGE_TRANSFER_DST_BIT)
                 : !m.MakeBuffer(m.slots[i], bytes, true, error)) {
            error = "ring slot " + std::to_string(i) + ": " + error;
            free_all();
            return false;
        }
    m.slot_count = slots;
    return true;
}

bool VulkanRing::Pack(void* list, uint32_t slot, uint32_t data_start, const std::vector<Packer::Texture>& textures,
                      std::string& error) {
    Impl& m = *impl_;
    auto cb = (VkCommandBuffer)list;
    if (slot >= m.slot_count || !cb || textures.empty()) {
        error = "no such slot, command buffer or textures";
        return false;
    }
    for (VkImageView v : m.views[slot])  // its last pack has run: the slot came free
        m.vkDestroyImageView(m.dev.device, v, nullptr);
    m.views[slot].clear();
    m.vkResetEvent(m.dev.device, m.events[slot]);
    struct Pass {
        uint32_t constants[8];
        VkDescriptorSet set;
        uint32_t groups_x, groups_y;
    };
    std::vector<Pass> passes;
    std::vector<VkImageMemoryBarrier> into;
    auto layout_for = [](const VulkanTexture* t) { return Readable(t->layout) ? t->layout : VK_IMAGE_LAYOUT_GENERAL; };
    auto readable = [&](const VulkanTexture* t) {
        if (!t || Readable(t->layout))
            return;
        for (const VkImageMemoryBarrier& b : into)
            if (b.image == t->image)
                return;
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = t->layout;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image;
        b.subresourceRange = t->range;
        if (HasStencil(t->format))  // a depth-stencil image's layout changes with both aspects
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        into.push_back(b);
    };
    for (const Packer::Texture& t : textures) {
        const auto* tex = (const VulkanTexture*)t.texture;
        const auto* ref = (const VulkanTexture*)t.reference;
        Channels c;
        if (!tex || !DescribeVulkan(tex->format, tex->width, tex->height, c, error))
            return false;
        const VkImageView view = m.View(*tex, error);
        if (!view)
            return false;
        m.views[slot].push_back(view);
        VkImageView ref_view = view;
        if (ref) {
            if (!(ref_view = m.View(*ref, error)))
                return false;
            m.views[slot].push_back(ref_view);
        }
        readable(tex);
        readable(ref);
        const VkDescriptorSet set = m.sets[m.next_set];
        m.next_set = (m.next_set + 1) % TABLES;
        const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, view, layout_for(tex)},
                                                 {VK_NULL_HANDLE, ref_view, layout_for(ref ? ref : tex)}};
        const VkDescriptorBufferInfo buffers[3] = {{m.slots[slot].buffer, 0, VK_WHOLE_SIZE},
                                                   {m.status.buffer, 0, VK_WHOLE_SIZE},
                                                   {m.counter.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[BINDINGS] = {};
        for (uint32_t i = 0; i < BINDINGS; i++) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1,
                         i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
            if (i < 2)
                writes[i].pImageInfo = &images[i];
            else
                writes[i].pBufferInfo = &buffers[i - 2];
        }
        m.vkUpdateDescriptorSets(m.dev.device, BINDINGS, writes, 0, nullptr);
        const bool srgb_view = tex->format == VK_FORMAT_R8G8B8A8_SRGB || tex->format == VK_FORMAT_B8G8R8A8_SRGB;
        passes.push_back({{c.width, c.height, c.kinds,
                           (c.depth ? REC_DEPTH : 0u) | (ref ? REC_REFERENCE : 0u) | (srgb_view ? REC_SRGB : 0u),
                           t.table, c.channels, 0, slot},
                          set,
                          ((c.width + 7) / 8 + 31) / 32,
                          (c.height + 7) / 8});
    }
    const uint32_t serial = ++m.serial ? m.serial : ++m.serial;  // 0 is never a serial
    // The game's writes to the images (and our last reads of the counter) before the kernels; the images that are not
    // in a sampled layout go to GENERAL.
    const VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    m.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before,
                           0, nullptr, (uint32_t)into.size(), into.data());
    if (m.queries) {
        m.vkCmdResetQueryPool(cb, m.queries, 2 * slot, 2);
        m.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m.queries, 2 * slot);
    }
    const VkMemoryBarrier between = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    const uint32_t init[8] = {0, 0, 0, 0, 0, 0, data_start, slot};
    m.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.init);
    m.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipeline_layout, 0, 1, &passes[0].set, 0, nullptr);
    m.vkCmdPushConstants(cb, m.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, init);
    m.vkCmdDispatch(cb, 1, 1, 1);
    m.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                           &between, 0, nullptr, 0, nullptr);
    m.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.pack);
    for (const Pass& p : passes) {
        m.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipeline_layout, 0, 1, &p.set, 0, nullptr);
        m.vkCmdPushConstants(cb, m.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, p.constants);
        m.vkCmdDispatch(cb, p.groups_x, p.groups_y, 1);
    }
    m.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                           &between, 0, nullptr, 0, nullptr);
    if (m.queries)
        m.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m.queries, 2 * slot + 1);
    const uint32_t done[8] = {0, 0, 0, 0, 0, 0, serial, slot};
    m.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.done);
    m.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipeline_layout, 0, 1, &passes[0].set, 0, nullptr);
    m.vkCmdPushConstants(cb, m.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, done);
    m.vkCmdDispatch(cb, 1, 1, 1);
    // The slot's and status's words to the host, then the event the host polls (Done); the images back to their
    // layouts after the reads.
    for (VkImageMemoryBarrier& b : into) {
        std::swap(b.oldLayout, b.newLayout);
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
    const VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_HOST_READ_BIT};
    m.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr,
                           (uint32_t)into.size(), into.data());
    m.vkCmdSetEvent(cb, m.events[slot], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    m.expected[slot] = serial;
    return true;
}

int VulkanRing::Done(uint32_t slot, uint64_t& bytes, double& gpu_ms) {
    Impl& m = *impl_;
    if (slot >= MAX_SLOTS || !m.expected[slot])
        return -1;
    if (m.vkGetEventStatus(m.dev.device, m.events[slot]) != VK_EVENT_SET)
        return 0;
    m.Invalidate(m.status);
    uint32_t words[2];
    std::memcpy(words, m.status.mapped + 8ull * slot, 8);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (words[1] != m.expected[slot])
        return 0;
    bytes = 4ull * words[0];
    gpu_ms = 0;
    uint64_t ticks[2] = {};
    if (m.queries && m.vkGetQueryPoolResults(m.dev.device, m.queries, 2 * slot, 2, sizeof(ticks), ticks, 8,
                                             VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        ticks[1] > ticks[0])
        gpu_ms = (double)(ticks[1] - ticks[0]) * m.timestamp_ns * 1e-6;
    m.expected[slot] = 0;
    return 1;
}

const uint8_t* VulkanRing::Read(uint32_t slot, uint64_t bytes, std::string& error) {
    Impl& m = *impl_;
    if (slot >= m.slot_count || bytes > m.slots[slot].bytes) {
        error = "RECORD: a slot's words beyond its buffer";
        return nullptr;
    }
    if (!m.copy_queue) {
        m.Invalidate(m.slots[slot]);
        return m.slots[slot].mapped;
    }
    // The pack made its writes available to the host before the event the copier saw (Done); this submission makes
    // them visible to our queue.
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkBufferCopy region = {0, 0, bytes};
    const VkMemoryBarrier to_host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_ACCESS_HOST_READ_BIT};
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m.copy_cb;
    if (m.vkBeginCommandBuffer(m.copy_cb, &begin) != VK_SUCCESS) {
        error = "RECORD: the copy's command buffer";
        return nullptr;
    }
    m.vkCmdCopyBuffer(m.copy_cb, m.slots[slot].buffer, m.readback[slot].buffer, 1, &region);
    m.vkCmdPipelineBarrier(m.copy_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0,
                           nullptr, 0, nullptr);
    if (m.vkEndCommandBuffer(m.copy_cb) != VK_SUCCESS ||
        m.vkQueueSubmit(m.copy_queue, 1, &submit, m.copy_fence) != VK_SUCCESS ||
        m.vkWaitForFences(m.dev.device, 1, &m.copy_fence, VK_TRUE, 10000000000ull) != VK_SUCCESS) {
        error = "RECORD: a copy on the recorder's queue failed";
        return nullptr;
    }
    m.vkResetFences(m.dev.device, 1, &m.copy_fence);
    m.Invalidate(m.readback[slot]);
    return m.readback[slot].mapped;
}

}  // namespace record
