#pragma once
#include "platform/gpu_frames.h"
#include <array>
#include <cstdio>
#include <vulkan/vulkan.h>

namespace remix::forward {
  // Optional private diagnostics. selectFrame must follow the slot's submission fence.
  // Ranges never overlap and preserve draw order; exhausted capacity is reported.
  class FragmentStatistics {
  public:
    static constexpr uint32_t viewModel = 6, viewModelEarly = 7;
    // The logical device must enable the supported pipelineStatisticsQuery and
    // occlusionQueryPrecise features (the native runtime does this at creation).
    FragmentStatistics(VkPhysicalDevice physical, VkDevice device);
    ~FragmentStatistics();
    void selectFrame(uint32_t slot);
    void reset(VkCommandBuffer command, VkExtent2D extent, VkSampleCountFlagBits samples, uint64_t bakeHash);
    void begin(VkCommandBuffer command, uint32_t category);
    void end(VkCommandBuffer command);
    bool enabled() const { return m_output != nullptr; }
  private:
    static constexpr uint32_t capacity = 256;
    struct Frame {
      VkQueryPool fragments = VK_NULL_HANDLE, samples = VK_NULL_HANDLE;
      std::array<uint32_t, capacity> categories{};
      uint32_t used = 0, omitted = 0;
      VkExtent2D extent{};
      uint32_t sampleCount = 1;
      uint64_t serial = 0, bakeHash = 0;
      bool active = false;
    };
    VkDevice m_device;
    std::FILE *m_output = nullptr;
    std::array<Frame, gpuFrameCount> m_frames{};
    uint32_t m_slot = 0;
    uint64_t m_serial = 0;
  };
}
