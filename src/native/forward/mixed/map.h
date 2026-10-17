#pragma once
#include "forward/resources.h"

namespace remix::forward::mixed {
  // Filled once before shading. The published bake retains this image and the
  // caller retains its upload buffer through the submission's existing fence.
  class OcclusionMap {
  public:
    OcclusionMap(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                 VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT, uint32_t layers = 1);
    void upload(VkCommandBuffer command, VkBuffer source, VkDeviceSize offset) const;
    void clear(VkCommandBuffer command) const;
    VkDescriptorImageInfo descriptor() const;
    uint64_t bytes() const { return m_image.bytes; }

  private:
    void transfer(VkCommandBuffer command, VkBuffer source, VkDeviceSize offset) const;
    Image m_image;
    VkExtent2D m_extent;
    uint32_t m_layers;
  };
} // namespace remix::forward::mixed
