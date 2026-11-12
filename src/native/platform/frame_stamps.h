#pragma once
#include "platform/frame_latency.h"
#include "platform/gpu_frames.h"
#include <vector>
#include <vulkan/vulkan.h>

// Frame latency statistics, the GPU's part (frame_latency.h): when each
// frame's command buffer started and finished on the GPU, as host times. Two
// timestamps per frame slot, at the top of the pipe as the command buffer
// begins and at the bottom as it ends, converted with a calibration pair from
// VK_KHR_calibrated_timestamps (or EXT), renewed every second. Results are read
// after the slot's fence (begin), without waiting, like the GPU pass timers.
namespace remix::latency {
  class FrameStamps {
  public:
    FrameStamps() = default;
    FrameStamps(const FrameStamps &) = delete;
    FrameStamps &operator=(const FrameStamps &) = delete;
    ~FrameStamps() { shutdown(); }

    // calibrate: vkGetCalibratedTimestampsKHR or EXT; null without the extension.
    void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
                    PFN_vkGetCalibratedTimestampsKHR calibrate) {
      shutdown();
      m_device = device;
      m_calibrate = calibrate;
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(physical, &properties);
      uint32_t count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
      m_validBits = queueFamily < count ? families[queueFamily].timestampValidBits : 0;
      m_period = properties.limits.timestampPeriod;
    }
    // Before vkDestroyDevice, with the device idle.
    void shutdown() {
      for (auto &slot : m_slots) {
        if (slot.pool)
          vkDestroyQueryPool(m_device, slot.pool, nullptr);
        slot = {};
      }
      m_recording = nullptr;
      m_calibrated = false;
      m_device = VK_NULL_HANDLE;
      m_calibrate = nullptr;
    }
    bool supported() const { return m_device && m_calibrate && m_validBits; }

    // Recording thread, after the slot's fence and vkBeginCommandBuffer: hands
    // the slot's previous frame to the recorder, then stamps this one's start.
    void begin(VkCommandBuffer command, uint32_t slot, uint64_t frame, Recorder &recorder) {
      m_recording = nullptr;
      if (slot >= gpuFrameCount || !supported())
        return;
      auto &s = m_slots[slot];
      harvest(s, recorder);
      if (!s.pool) {
        const VkQueryPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
        if (vkCreateQueryPool(m_device, &info, nullptr, &s.pool) != VK_SUCCESS) {
          s.pool = VK_NULL_HANDLE;
          return;
        }
      }
      vkCmdResetQueryPool(command, s.pool, 0, 2);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.pool, 0);
      s.frame = frame;
      m_recording = &s;
      m_command = command;
    }
    // Before vkEndCommandBuffer.
    void end(VkCommandBuffer command) {
      if (!m_recording || command != m_command)
        return;
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_recording->pool, 1);
      m_recording->pending = true;
      m_recording = nullptr;
    }
    // Recording failed before end: the frame keeps no stamps.
    void abandon() { m_recording = nullptr; }

  private:
    struct Slot {
      VkQueryPool pool = VK_NULL_HANDLE;
      uint64_t frame = 0;
      bool pending = false;
    };
    void harvest(Slot &s, Recorder &recorder) {
      if (!s.pending || !s.pool)
        return;
      s.pending = false;
      uint64_t values[4] = {}; // value and availability per query; no WAIT: the fence has signaled
      if (vkGetQueryPoolResults(m_device, s.pool, 0, 2, sizeof(values), values, 2 * sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS ||
          !values[1] || !values[3] || !calibrate())
        return;
      recorder.mark(s.frame, GpuStart,
                    gpuToHost(values[0], m_calibrationTick, m_calibrationHost, m_validBits, m_period));
      recorder.mark(s.frame, GpuEnd, gpuToHost(values[2], m_calibrationTick, m_calibrationHost, m_validBits, m_period));
    }
    // A fresh calibration pair once a second (the clocks drift apart slowly).
    bool calibrate() {
      const int64_t now = hostNanoseconds();
      if (m_calibrated && now - m_calibratedAt < 1000000000)
        return true;
#if defined(_WIN32)
      constexpr VkTimeDomainKHR hostDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
      constexpr VkTimeDomainKHR hostDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif
      const VkCalibratedTimestampInfoKHR infos[2] = {
          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR},
          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = hostDomain}};
      uint64_t stamps[2] = {}, deviation = 0;
      if (m_calibrate(m_device, 2, infos, stamps, &deviation) != VK_SUCCESS)
        return m_calibrated; // keep the last pair
      m_calibrationTick = stamps[0];
      m_calibrationHost = hostNanosecondsFromDomain(stamps[1]);
      m_calibratedAt = now;
      m_calibrated = true;
      return true;
    }
    VkDevice m_device = VK_NULL_HANDLE;
    PFN_vkGetCalibratedTimestampsKHR m_calibrate = nullptr;
    uint32_t m_validBits = 0;
    double m_period = 0;
    Slot m_slots[gpuFrameCount];
    Slot *m_recording = nullptr;
    VkCommandBuffer m_command = VK_NULL_HANDLE;
    bool m_calibrated = false;
    uint64_t m_calibrationTick = 0;
    int64_t m_calibrationHost = 0, m_calibratedAt = 0;
  };
} // namespace remix::latency
