#include "forward/mixed/map.h"

namespace remix::forward::mixed {
  OcclusionMap::OcclusionMap(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent)
      : m_extent(extent) {
    m_image.create(physical, device, extent, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
  }
  void OcclusionMap::upload(VkCommandBuffer command, VkBuffer source, VkDeviceSize offset) const {
    transfer(command, source, offset);
  }
  void OcclusionMap::clear(VkCommandBuffer command) const { transfer(command, VK_NULL_HANDLE, 0); }
  void OcclusionMap::transfer(VkCommandBuffer command, VkBuffer source, VkDeviceSize offset) const {
    VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = m_image.handle,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    if (source) {
      const VkBufferImageCopy copy{.bufferOffset = offset,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {m_extent.width, m_extent.height, 1}};
      vkCmdCopyBufferToImage(command, source, m_image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
      const VkClearColorValue zero{};
      vkCmdClearColorImage(command, m_image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                           &barrier.subresourceRange);
    }
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
  }
  VkDescriptorImageInfo OcclusionMap::descriptor() const {
    return {VK_NULL_HANDLE, m_image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
} // namespace remix::forward::mixed
