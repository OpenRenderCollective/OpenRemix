#pragma once
#include <memory>
#include <vulkan/vulkan.h>

namespace remix {
  // GPU-only copy of the game image, excluding OpenRemix's live overlays.
  // Submission slots keep it alive when resize replaces the capture target.
  class FrameSnapshot {
  public:
    static std::shared_ptr<FrameSnapshot> create(VkPhysicalDevice physical, VkDevice device, VkFormat format,
                                                 VkExtent2D extent);
    ~FrameSnapshot();
    bool matches(VkFormat format, VkExtent2D extent) const;
    void capture(VkCommandBuffer command, VkImage source, VkImageLayout layout);
    void restore(VkCommandBuffer command, VkImage destination, VkExtent2D extent, VkImageLayout layout) const;

  private:
    VkDevice m_device{};
    VkImage m_image{};
    VkDeviceMemory m_memory{};
    VkFormat m_format{};
    VkExtent2D m_extent{};
    bool m_captured = false;
  };
} // namespace remix
