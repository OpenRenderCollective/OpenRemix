#pragma once
#include "forward/resources.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace remix::forward {
  // Optional, one-frame developer readback. The owner selects a slot only after
  // its fence; pending buffers stay alive until that slot completes.
  class ImageDump {
  public:
    static bool enabled(VkPhysicalDevice physical);
    ImageDump(VkPhysicalDevice physical, VkDevice device);
    bool fullImages() const { return m_fullImages; }
    void selectFrame(uint32_t slot) noexcept;
    void record(VkCommandBuffer command, uint32_t slot, const char *name, VkImage image, VkFormat format,
                VkExtent2D extent, VkImageLayout layout, uint32_t mip = 0) noexcept;
    void recordBuffer(VkCommandBuffer command, uint32_t slot, const char *name,
                      VkDescriptorBufferInfo source) noexcept;

  private:
    struct Readback {
      std::string name;
      uint32_t metadata[7]; // format, full width/height, crop X/Y, crop width/height
      std::unique_ptr<Buffer> buffer;
    };
    struct BufferReadback {
      std::string name;
      std::unique_ptr<Buffer> buffer;
      size_t bytes;
    };
    bool active(uint32_t slot);
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    std::string m_prefix;
    std::optional<uint32_t> m_slot;
    bool m_done = false;
    bool m_fullImages = false;
    std::vector<Readback> m_pending;
    std::vector<BufferReadback> m_buffers;
  };
} // namespace remix::forward
