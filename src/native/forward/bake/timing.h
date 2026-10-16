#pragma once
#include "platform/gpu_frames.h"
#include <array>
#include <cmath>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

namespace remix::forward::bake {
  // One timestamp pair per owning frame slot. The caller observes that slot's
  // fence before reading or resetting it. No query wait or extra submission.
  class Timing {
  public:
    Timing(VkPhysicalDevice physical, VkDevice device, uint32_t family) : m_device(device) {
      if (family == VK_QUEUE_FAMILY_IGNORED)
        return;
      uint32_t count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
      if (family >= count || !families[family].timestampValidBits)
        return;
      m_bits = families[family].timestampValidBits;
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical, &properties);
      m_period = properties.limits.timestampPeriod;
      if (!std::isfinite(m_period) || m_period <= 0)
        return;
      const VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = gpuFrameCount * 2};
      const auto result = vkCreateQueryPool(device, &info, nullptr, &m_pool);
      if (result == VK_ERROR_DEVICE_LOST)
        throw result;
      if (result != VK_SUCCESS)
        m_pool = VK_NULL_HANDLE;
    }
    Timing(const Timing &) = delete;
    Timing &operator=(const Timing &) = delete;
    ~Timing() {
      if (m_pool)
        vkDestroyQueryPool(m_device, m_pool, nullptr);
    }
    bool supported() const { return m_pool != VK_NULL_HANDLE; }
    void begin(VkCommandBuffer command, uint32_t slot) {
      if (!m_pool)
        return;
      vkCmdResetQueryPool(command, m_pool, slot * 2, 2);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, slot * 2);
      m_pending.at(slot) = true;
    }
    void end(VkCommandBuffer command, uint32_t slot) {
      if (m_pool) {
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, slot * 2 + 1);
      }
    }
    std::optional<double> elapsed(uint32_t slot) const {
      if (!m_pool || !m_pending.at(slot))
        return {};
      struct Query {
        uint64_t value, available;
      };
      std::array<Query, 2> queries{};
      const auto result =
          vkGetQueryPoolResults(m_device, m_pool, slot * 2, 2, sizeof(queries), queries.data(), sizeof(Query),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      if (result == VK_ERROR_DEVICE_LOST)
        throw result;
      if (result != VK_SUCCESS || !queries[0].available || !queries[1].available)
        return {};
      const uint64_t mask = m_bits == 64 ? ~uint64_t(0) : (uint64_t(1) << m_bits) - 1;
      return double((queries[1].value - queries[0].value) & mask) * m_period / 1e6;
    }

  private:
    VkDevice m_device;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    uint32_t m_bits = 0;
    float m_period = 0;
    std::array<bool, gpuFrameCount> m_pending{};
  };
} // namespace remix::forward::bake
