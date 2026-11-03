#pragma once
#include "forward/resources.h"
#include "forward/shadows/sun_cache.h"

namespace remix::forward::shadows {
  // Queue-ordered frozen/moving cascade pairs. The renderer borrows layer
  // targets for its existing draw schedule and retires this owner after fences.
  // The compatible depth render pass is shared with the local-shadow provider.
  class SunMaps {
  public:
    ~SunMaps();
    void initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass pass, uint32_t resolution);
    std::array<bool, cascadeCount> update(const std::shared_ptr<const scene::BakeScene> &source,
                                         bool enabled, std::array<Matrix, cascadeCount> &matrices) {
      return m_cache.update(source, enabled, matrices, m_resolution);
    }
    VkImageView view() const { return m_image.view; }
    VkFramebuffer framebuffer(uint32_t layer) const { return m_targets[layer]; }
    uint64_t memoryBytes() const { return m_image.bytes; }

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_resolution = 0;
    Image m_image;
    std::array<VkImageView, sunShadowLayers> m_views{};
    std::array<VkFramebuffer, sunShadowLayers> m_targets{};
    SunCache m_cache;
  };
} // namespace remix::forward::shadows
