#include "runtime/frame_snapshot.h"

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
} // namespace remix
