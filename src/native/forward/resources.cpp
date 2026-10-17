#include "forward/resources.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <cstring>

namespace remix::forward {
  void check(VkResult result) {
    if (result != VK_SUCCESS)
      throw result;
  }
  uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }
  Buffer::~Buffer() {
    if (mapped)
      vkUnmapMemory(device, memory);
    if (handle)
      vkDestroyBuffer(device, handle, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
  }
  void Buffer::create(VkPhysicalDevice physical, VkDevice target, VkDeviceSize bytes,
                      VkBufferUsageFlags usage, Memory placement) {
    device = target;
    capacity = std::max<VkDeviceSize>(bytes, 16);
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = capacity,
                                  .usage = usage,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateBuffer(device, &info, nullptr, &handle));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, handle, &requirements);
    allocationBytes = requirements.size;
    uint32_t type;
    if (placement == Memory::Device) {
      type = memoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } else {
      try {
        type = memoryType(physical, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        coherent = true;
      } catch (VkResult) {
        type = memoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      }
    }
    const VkMemoryAllocateFlagsInfo flags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                          .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT ? &flags : nullptr,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type};
    check(vkAllocateMemory(device, &allocation, nullptr, &memory));
    check(vkBindBufferMemory(device, handle, memory, 0));
    if (placement == Memory::Host)
      check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
  }
  void Buffer::write(const void *data, size_t bytes, size_t offset) {
    if (!mapped)
      throw VK_ERROR_MEMORY_MAP_FAILED;
    if (offset > capacity || bytes > capacity - offset)
      throw VK_ERROR_OUT_OF_HOST_MEMORY;
    if (bytes)
      std::memcpy(static_cast<std::byte *>(mapped) + offset, data, bytes);
    if (!coherent) {
      const VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                      .memory = memory,
                                      .offset = 0,
                                      .size = VK_WHOLE_SIZE};
      check(vkFlushMappedMemoryRanges(device, 1, &range));
    }
  }
  void Buffer::read(void *data, size_t bytes) const {
    if (!mapped)
      throw VK_ERROR_MEMORY_MAP_FAILED;
    if (bytes > capacity)
      throw VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!coherent) {
      const VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                      .memory = memory,
                                      .offset = 0,
                                      .size = VK_WHOLE_SIZE};
      check(vkInvalidateMappedMemoryRanges(device, 1, &range));
    }
    if (bytes)
      std::memcpy(data, mapped, bytes);
  }
  Image::~Image() {
    if (view)
      vkDestroyImageView(device, view, nullptr);
    if (handle)
      vkDestroyImage(device, handle, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
  }
  void Image::create(VkPhysicalDevice physical, VkDevice target, VkExtent2D extent, VkFormat format,
                     VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect,
                     uint32_t layers, uint32_t levels, VkImageViewType viewType) {
    device = target;
    const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = format,
                                 .extent = {extent.width, extent.height, 1},
                                 .mipLevels = levels,
                                 .arrayLayers = layers,
                                 .samples = samples,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = usage,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateImage(device, &info, nullptr, &handle));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, handle, &requirements);
    bytes = requirements.size;
    const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                          .allocationSize = bytes,
                                          .memoryTypeIndex = memoryType(physical, requirements.memoryTypeBits,
                                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &allocation, nullptr, &memory));
    check(vkBindImageMemory(device, handle, memory, 0));
    const VkImageViewCreateInfo viewInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = handle,
                                         .viewType = viewType != VK_IMAGE_VIEW_TYPE_MAX_ENUM ? viewType :
                                           layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                         .format = format,
                                         .subresourceRange = {aspect, 0, levels, 0, layers}};
    check(vkCreateImageView(device, &viewInfo, nullptr, &view));
  }
  VkShaderModule shaderModule(VkDevice device, const uint32_t *code, size_t bytes) {
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module;
    check(pipeline::createShaderModule(device, &info, nullptr, &module));
    return module;
  }
} // namespace remix::forward
