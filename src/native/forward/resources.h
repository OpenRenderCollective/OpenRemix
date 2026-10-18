#pragma once
#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace remix::forward {
  void check(VkResult result);
  uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags);
  struct Buffer {
    enum class Memory { Host, Device, Readback }; // uploads, device-only storage, CPU-cached results
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize capacity = 0;
    VkDeviceSize allocationBytes = 0;
    void *mapped = nullptr;
    bool coherent = false;
    ~Buffer();
    void create(VkPhysicalDevice physical, VkDevice device, VkDeviceSize bytes, VkBufferUsageFlags usage,
                Memory memory = Memory::Host);
    void write(const void *data, size_t bytes, size_t offset = 0);
    // Publish writes made directly through mapped, after checking capacity.
    void flush();
    // Caller has made GPU writes visible to HOST and waited for their fence.
    void read(void *data, size_t bytes) const;
    VkDescriptorBufferInfo descriptor() const { return {handle, 0, capacity}; }
  };
  struct Image {
    Image() = default;
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    VkDevice device = VK_NULL_HANDLE;
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
    ~Image();
    void create(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent, VkFormat format,
                VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect,
                uint32_t layers = 1, uint32_t levels = 1,
                VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM);
  };
  VkShaderModule shaderModule(VkDevice device, const uint32_t *code, size_t bytes);
} // namespace remix::forward
