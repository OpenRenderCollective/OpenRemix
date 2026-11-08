#include "runtime/frame_snapshot.h"
#include <cstring>

namespace remix {
  namespace {
    void check(VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    }
    VkImageMemoryBarrier barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source,
                                 VkAccessFlags destination) {
      return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .srcAccessMask = source,
              .dstAccessMask = destination,
              .oldLayout = from,
              .newLayout = to,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = image,
              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
  } // namespace
  std::shared_ptr<FrameSnapshot> FrameSnapshot::create(VkPhysicalDevice physical, VkDevice device, VkFormat format,
                                                       VkExtent2D extent) {
    auto result = std::make_shared<FrameSnapshot>();
    result->m_device = device;
    result->m_format = format;
    result->m_extent = extent;
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = format,
                                  .extent = {extent.width, extent.height, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    check(vkCreateImage(device, &image, nullptr, &result->m_image));
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, result->m_image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t type = 0;
    for (; type < memory.memoryTypeCount; ++type)
      if ((requirements.memoryTypeBits & (1u << type)) &&
          (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        break;
    if (type == memory.memoryTypeCount)
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type};
    check(vkAllocateMemory(device, &allocation, nullptr, &result->m_memory));
    check(vkBindImageMemory(device, result->m_image, result->m_memory, 0));
    return result;
  }
  FrameSnapshot::~FrameSnapshot() {
    if (m_image)
      vkDestroyImage(m_device, m_image, nullptr);
    if (m_memory)
      vkFreeMemory(m_device, m_memory, nullptr);
  }
  bool FrameSnapshot::matches(VkFormat format, VkExtent2D extent) const {
    return m_format == format && m_extent.width == extent.width && m_extent.height == extent.height;
  }
  void FrameSnapshot::capture(VkCommandBuffer command, VkImage source, VkImageLayout layout) {
    VkImageMemoryBarrier barriers[] = {
        barrier(source, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT),
        barrier(m_image, m_captured ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, m_captured ? VK_ACCESS_TRANSFER_READ_BIT : 0,
                VK_ACCESS_TRANSFER_WRITE_BIT)};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    const VkImageCopy copy{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .extent = {m_extent.width, m_extent.height, 1}};
    vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &copy);
    barriers[0] = barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    barriers[1] = barrier(m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 2, barriers);
    m_captured = true;
  }
  void FrameSnapshot::restore(VkCommandBuffer command, VkImage destination, VkExtent2D extent,
                              VkImageLayout layout) const {
    auto transition = barrier(destination, layout == VK_IMAGE_LAYOUT_GENERAL ? layout : VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &transition);
    const VkImageBlit blit{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .srcOffsets = {{0, 0, 0}, {int32_t(m_extent.width), int32_t(m_extent.height), 1}},
                           .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .dstOffsets = {{0, 0, 0}, {int32_t(extent.width), int32_t(extent.height), 1}}};
    vkCmdBlitImage(command, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    transition = barrier(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &transition);
  }
  std::vector<uint8_t> FrameSnapshot::read(VkPhysicalDevice physical, VkQueue queue, uint32_t family) const {
    if (!m_captured)
      return {};
    const VkDeviceSize texel = m_format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    const VkDeviceSize size = VkDeviceSize(m_extent.width) * m_extent.height * texel;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    VkFence fence{};
    std::vector<uint8_t> texels;
    bool submitted = false;
    try {
      const VkBufferCreateInfo info{
          .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      check(vkCreateBuffer(m_device, &info, nullptr, &buffer));
      VkMemoryRequirements requirements{};
      vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
      VkPhysicalDeviceMemoryProperties properties{};
      vkGetPhysicalDeviceMemoryProperties(physical, &properties);
      const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      uint32_t type = 0;
      for (; type < properties.memoryTypeCount; ++type)
        if ((requirements.memoryTypeBits & (1u << type)) && (properties.memoryTypes[type].propertyFlags & host) == host)
          break;
      if (type == properties.memoryTypeCount)
        throw VK_ERROR_FEATURE_NOT_PRESENT;
      const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                            .allocationSize = requirements.size,
                                            .memoryTypeIndex = type};
      check(vkAllocateMemory(m_device, &allocation, nullptr, &memory));
      check(vkBindBufferMemory(m_device, buffer, memory, 0));
      const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                             .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                             .queueFamilyIndex = family};
      check(vkCreateCommandPool(m_device, &poolInfo, nullptr, &pool));
      VkCommandBuffer command{};
      const VkCommandBufferAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                 .commandPool = pool,
                                                 .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                 .commandBufferCount = 1};
      check(vkAllocateCommandBuffers(m_device, &allocate, &command));
      const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      check(vkBeginCommandBuffer(command, &begin));
      // The capture's write, from an earlier submission, made visible to this copy.
      const VkMemoryBarrier written{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &written, 0,
                           nullptr, 0, nullptr);
      const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {m_extent.width, m_extent.height, 1}};
      vkCmdCopyImageToBuffer(command, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
      const VkMemoryBarrier readable{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &readable, 0,
                           nullptr, 0, nullptr);
      check(vkEndCommandBuffer(command));
      const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(vkCreateFence(m_device, &fenceInfo, nullptr, &fence));
      const VkSubmitInfo submit{
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
      check(vkQueueSubmit(queue, 1, &submit, fence));
      submitted = true;
      check(vkWaitForFences(m_device, 1, &fence, VK_TRUE, 5'000'000'000ull));
      void *mapped = nullptr;
      check(vkMapMemory(m_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
      texels.resize(size_t(size));
      std::memcpy(texels.data(), mapped, texels.size());
      vkUnmapMemory(m_device, memory);
    } catch (VkResult) {
      texels.clear();
      if (submitted)
        vkQueueWaitIdle(queue); // the copy may still run: the objects outlive it
    }
    if (fence)
      vkDestroyFence(m_device, fence, nullptr);
    if (pool)
      vkDestroyCommandPool(m_device, pool, nullptr);
    if (buffer)
      vkDestroyBuffer(m_device, buffer, nullptr);
    if (memory)
      vkFreeMemory(m_device, memory, nullptr);
    return texels;
  }
} // namespace remix
