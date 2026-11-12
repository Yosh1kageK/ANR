// The recording core's ring in Vulkan games (recorder.h Ring): record_pack.slang's SPIR-V on the game's command buffer.
// With a queue of the recorder's own on the game's device (VulkanDevice::copy_family, created at vkCreateDevice by a
// queue provider, NrRecordVulkanQueue), the pack writes slots in device memory and that queue copies each finished
// slot to host memory, as D3D12's copy queue does. Without one (Vulkan gives no queue of our own on a device the game
// made) the pack writes its slot straight into host memory, which costs the game's queue the transfer. Textures are
// VulkanTexture, lists VkCommandBuffer. Windows, natively or under Wine (winevulkan); the Vulkan commands come from
// the game's vkGetDeviceProcAddr.
//
//   auto ring = record::VulkanRing::Make(device, kernel_dir, error);   // kernel_dir: the three .spv files
//   record::VulkanTexture t = {image, format, width, height, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
//   ... record::Recorder::Start(ring, ...), then Frame(command_buffer, n, inputs with &t)
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <memory>
#include <string>

#include "record/recorder.h"

namespace record {

// A game image and the layout it is in during the call (and is left in).
struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageSubresourceRange range = {};  // the source's view's: its first mip level and layer are coded
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};

// The game's device and its entry points (gipa may be the loader's).
struct VulkanDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    // A queue the game does not use (its family and index), for the copies; copy_family UINT32_MAX: none.
    uint32_t copy_family = UINT32_MAX, copy_index = 0;
};

// What a queue provider exports (any module in the process): the queue it added for the recorder to `device` at
// vkCreateDevice, or false.
using VulkanQueueQuery = bool (*)(VkDevice device, uint32_t* family, uint32_t* index);
constexpr const char* VULKAN_QUEUE_EXPORT = "NrRecordVulkanQueue";

// Channels of an image format (FORMAT.md "kinds") and the view format the kernels read it through; false if the coding
// has no lossless view of it. The recorded vk_format is the format itself, a depth format's depth plane as D32 (and
// D16 as R16).
bool DescribeVulkan(VkFormat format, uint32_t width, uint32_t height, Channels& out, std::string& error);

class VulkanRing : public Ring {
public:
    // kernel_dir holds record_pack.slang.record_init.spv, .record_pack.spv and .record_done.spv (//record:kernels_vk).
    static std::shared_ptr<VulkanRing> Make(const VulkanDevice& device, const std::string& kernel_dir,
                                            std::string& error);
    ~VulkanRing() override;
    VkDevice device() const;
    bool copies() const;  // slots in device memory, copied on the recorder's queue
    bool Describe(const void* texture, Channels& out, std::string& error) override;
    bool Create(unsigned slots, uint64_t bytes, std::string& error) override;
    bool Pack(void* list, uint32_t slot, uint32_t data_start, const std::vector<Packer::Texture>& textures,
              std::string& error) override;
    int Done(uint32_t slot, uint64_t& bytes, double& gpu_ms) override;
    const uint8_t* Read(uint32_t slot, uint64_t bytes, std::string& error) override;
    void Release(uint32_t) override {}

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace record
