#pragma once
#include "platform/gpu_frames.h"
#include "platform/gpu_timings.h"
#include <vulkan/vulkan.h>

// GPU pass timers ("rtx.native.gpuPassTimers", default off): timestamp ranges
// around every pass of a frame's command buffer. The runtime owns one
// GpuTimers; while it records a frame with the setting on, that recorder is
// the recording thread's current one and GpuScope (any backend) adds a range
// to it. With the setting off nothing is current, and a scope records nothing.
//
// Every timestamp is written at VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT: it is
// latched once all previously recorded work on the queue has completed, so a
// range measures elapsed completion time between its boundaries. These writes
// do not block subsequent commands: pipeline stages from neighbouring passes
// can overlap, so a child range is not an isolated cost for its draw calls.
// Compare whole-frame time and workload counters when another pass changes.
// Results are read after the frame slot's fence
// (beginFrame), without waiting: a slot whose queries are not all available
// is skipped.
namespace remix::gpu {
  class GpuTimers;
  // The recorder of the frame the calling thread is recording, or null.
  inline thread_local GpuTimers *currentTimers = nullptr;

  class GpuTimers {
  public:
    static constexpr uint32_t maxPasses = 127;
    static constexpr uint32_t queryCount = 2 * maxPasses + 2;
    GpuTimers() = default;
    GpuTimers(const GpuTimers &) = delete;
    GpuTimers &operator=(const GpuTimers &) = delete;
    ~GpuTimers() { shutdown(); }

    void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily) {
      shutdown();
      m_device = device;
      m_queueFamily = queueFamily;
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
      if (currentTimers == this)
        currentTimers = nullptr;
      for (auto &slot : m_slots) {
        if (slot.pool)
          vkDestroyQueryPool(m_device, slot.pool, nullptr);
        slot = {};
      }
      m_latest = {};
      m_open.clear();
      m_recording = nullptr;
      m_device = VK_NULL_HANDLE;
    }
    bool supported() const { return m_device && m_validBits != 0; }

    // Recording thread, after the slot's fence and vkBeginCommandBuffer:
    // takes the slot's previous frame (never waits), then, when enabled,
    // starts timing this one and becomes the thread's current recorder.
    void beginFrame(VkCommandBuffer command, uint32_t slot, bool enabled, uint64_t frame, VkExtent2D render,
                    VkExtent2D output) {
      if (slot >= gpuFrameCount)
        return;
      auto &s = m_slots[slot];
      harvest(s);
      if (!enabled || !supported()) {
        m_latest = {};
        m_recording = nullptr;
        if (currentTimers == this)
          currentTimers = nullptr;
        return;
      }
      if (!s.pool) {
        const VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                         .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                         .queryCount = queryCount};
        if (vkCreateQueryPool(m_device, &info, nullptr, &s.pool) != VK_SUCCESS) {
          s.pool = VK_NULL_HANDLE;
          return;
        }
      }
      vkCmdResetQueryPool(command, s.pool, 0, queryCount);
      s.ranges.clear();
      s.queries = 1;
      s.truncated = 0;
      s.frame = frame;
      s.render = render;
      s.output = output;
      m_open.clear();
      m_recording = &s;
      m_command = command;
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, 0);
      currentTimers = this;
    }
    // Before vkEndCommandBuffer: closes open ranges and the frame.
    void endFrame(VkCommandBuffer command) {
      if (currentTimers == this)
        currentTimers = nullptr;
      if (!m_recording || command != m_command)
        return;
      while (!m_open.empty())
        end(command, m_open.back());
      auto &s = *m_recording;
      s.end = s.queries++;
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, s.end);
      s.pending = true;
      m_recording = nullptr;
    }
    // Recording failed before endFrame: the frame keeps no results.
    void abandon() {
      if (currentTimers == this)
        currentTimers = nullptr;
      if (m_recording)
        m_recording->pending = false;
      m_recording = nullptr;
      m_open.clear();
    }

    // A range in the frame's command buffer; noPass when not recorded.
    uint32_t begin(VkCommandBuffer command, const char *name, uint32_t width, uint32_t height) {
      if (!m_recording || command != m_command)
        return noPass;
      auto &s = *m_recording;
      if (s.ranges.size() >= maxPasses) {
        ++s.truncated;
        return noPass;
      }
      PassRange range;
      for (size_t i = 0; name && name[i] && i + 1 < sizeof(range.name); ++i)
        range.name[i] = name[i];
      range.width = width;
      range.height = height;
      range.parent = m_open.empty() ? noPass : m_open.back();
      range.depth = uint32_t(m_open.size());
      range.begin = range.end = s.queries++;
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, range.begin);
      const auto id = uint32_t(s.ranges.size());
      s.ranges.push_back(range);
      m_open.push_back(id);
      return id;
    }
    void end(VkCommandBuffer command, uint32_t id) {
      if (!m_recording || command != m_command || id == noPass || id >= m_recording->ranges.size() ||
          m_recording->ranges[id].end != m_recording->ranges[id].begin)
        return;
      // Ranges close innermost first; closing an outer one closes those inside.
      while (!m_open.empty()) {
        const auto open = m_open.back();
        m_open.pop_back();
        auto &range = m_recording->ranges[open];
        range.end = m_recording->queries++;
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_recording->pool, range.end);
        if (open == id)
          break;
      }
    }
    // The newest completed frame (two frames old with two frame slots).
    const FrameTimings &latest() const { return m_latest; }

  private:
    struct Slot {
      VkQueryPool pool = VK_NULL_HANDLE;
      std::vector<PassRange> ranges;
      uint32_t queries = 0, end = 0, truncated = 0;
      uint64_t frame = 0;
      VkExtent2D render{}, output{};
      bool pending = false;
    };
    void harvest(Slot &s) {
      if (!s.pending || !s.pool)
        return;
      s.pending = false;
      // Value and availability per query; no WAIT: the fence has signaled.
      std::vector<uint64_t> values(size_t(s.queries) * 2);
      const auto result =
          vkGetQueryPoolResults(m_device, s.pool, 0, s.queries, values.size() * sizeof(uint64_t), values.data(),
                                2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      if (result != VK_SUCCESS)
        return;
      std::vector<uint64_t> stamps(s.queries);
      for (uint32_t i = 0; i < s.queries; ++i) {
        if (!values[2 * i + 1])
          return;
        stamps[i] = values[2 * i];
      }
      m_latest = resolve(s.ranges, stamps.data(), 0, s.end, m_validBits, m_period);
      m_latest.truncated = s.truncated;
      m_latest.queueFamily = m_queueFamily;
      m_latest.frame = s.frame;
      m_latest.renderWidth = s.render.width;
      m_latest.renderHeight = s.render.height;
      m_latest.outputWidth = s.output.width;
      m_latest.outputHeight = s.output.height;
    }
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0, m_validBits = 0;
    double m_period = 0;
    Slot m_slots[gpuFrameCount];
    Slot *m_recording = nullptr;
    VkCommandBuffer m_command = VK_NULL_HANDLE;
    std::vector<uint32_t> m_open;
    FrameTimings m_latest;
  };

  // A timed pass in the current frame, from construction to destruction (or
  // close()). Name and resolution are copied. No-op without a current recorder
  // or in another command buffer (e.g. a secondary).
  class GpuScope {
  public:
    GpuScope(VkCommandBuffer command, const char *name, uint32_t width = 0, uint32_t height = 0)
        : m_command(command) {
      if (auto *timers = currentTimers) {
        m_timers = timers;
        m_id = timers->begin(command, name, width, height);
      }
    }
    GpuScope(VkCommandBuffer command, const char *name, VkExtent2D extent)
        : GpuScope(command, name, extent.width, extent.height) {}
    GpuScope(const GpuScope &) = delete;
    GpuScope &operator=(const GpuScope &) = delete;
    ~GpuScope() { close(); }
    void close() {
      // The recorder may have finished the frame (endFrame closes open ranges).
      if (m_timers && m_timers == currentTimers)
        m_timers->end(m_command, m_id);
      m_timers = nullptr;
    }

  private:
    VkCommandBuffer m_command;
    GpuTimers *m_timers = nullptr;
    uint32_t m_id = noPass;
  };
  inline bool timing() { return currentTimers != nullptr; }
} // namespace remix::gpu
