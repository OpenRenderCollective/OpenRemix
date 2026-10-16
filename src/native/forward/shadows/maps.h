#pragma once
#include "forward/resources.h"
#include "forward/shadows/plan.h"
#include <functional>

namespace remix::forward::shadows {
  // Queue-ordered shared targets. The owner destroys these only after the
  // renderer's outstanding fences; each layer is initialized before sampling.
  class Maps {
  public:
    ~Maps();
    void initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass pass, uint32_t count,
                    uint32_t resolution);
    using Draw = std::function<void(uint32_t slot, uint32_t face, bool staticCasters)>;
    void record(VkCommandBuffer command, const Selection &selection, const Draw &draw);
    VkImageView staticView() const { return m_static.view; }
    VkImageView dynamicView() const { return m_dynamic.view; }
    uint64_t memoryBytes() const { return m_static.bytes + m_dynamic.bytes; }

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_pass = VK_NULL_HANDLE;
    uint32_t m_count = 0, m_resolution = 1, m_layers = faces;
    bool m_initialized = false;
    Image m_static, m_dynamic;
    std::vector<VkImageView> m_views;
    std::vector<VkFramebuffer> m_targets;
    void initializeLayouts(VkCommandBuffer command);
  };
} // namespace remix::forward::shadows
