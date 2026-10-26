#pragma once
#include <remix/remix_c.h>
#include <vulkan/vulkan.h>
#include <string>

namespace remix {
  // One on-demand readback. All Vulkan access is on the presentation thread.
  class PresentedScreenshot {
  public:
    remixapi_ErrorCode request(const char *path, bool supported);
    // `image` in `layout`, which it is left in: 8-bit RGBA or BGRA, or
    // RGBA16F (HDR10's back buffer, gamma encoded; clipped to [0, 1]).
    void record(VkPhysicalDevice physical, VkDevice device, VkCommandBuffer command, VkImage image,
                VkExtent2D extent, VkFormat format, VkImageLayout layout);
    void submitted(VkFence fence);
    void complete(VkDevice device, bool wait = false);
    void reset(VkDevice device, bool keepRequest = false);
    // A request the next record() will capture.
    bool pending() const { return !m_requested.empty() && m_captured.empty(); }

  private:
    std::string m_requested, m_captured;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkExtent2D m_extent = {};
    bool m_bgra = false, m_half = false;
    VkFence m_fence = VK_NULL_HANDLE;
  };
} // namespace remix
