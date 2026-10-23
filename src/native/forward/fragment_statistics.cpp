#include "forward/fragment_statistics.h"
#include <cstdlib>

namespace remix::forward {
  FragmentStatistics::FragmentStatistics(VkPhysicalDevice physical, VkDevice device) : m_device(device) {
    const char *path = std::getenv("REMIX_DEBUG_FORWARD_FRAGMENT_STATS");
    if (!path || !*path) return;
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(physical, &available);
    if (!available.pipelineStatisticsQuery || !available.occlusionQueryPrecise) {
      std::fprintf(stderr, "Forward fragment diagnostics require pipeline statistics and precise occlusion queries\n");
      return;
    }
    m_output = std::fopen(path, "a");
    if (!m_output) {
      std::fprintf(stderr, "Cannot open Forward fragment diagnostics: %s\n", path);
      return;
    }
    // Append allows target recreation without losing earlier frames.
    std::fprintf(m_output, "frame,bake_hash,width,height,msaa,category,fragment_invocations,passed_samples,"
                           "available,omitted_ranges,input_primitives,vertex_invocations\n");
    for (auto &frame : m_frames) {
      const VkQueryPoolCreateInfo fragments{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
          .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS, .queryCount = capacity,
          .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
      const VkQueryPoolCreateInfo samples{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
          .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = capacity};
      // Diagnostic allocation failure must not prevent ordinary rendering.
      if (vkCreateQueryPool(device, &fragments, nullptr, &frame.fragments) != VK_SUCCESS ||
          vkCreateQueryPool(device, &samples, nullptr, &frame.samples) != VK_SUCCESS) {
        std::fprintf(stderr, "Cannot allocate Forward fragment query pools\n");
        std::fclose(m_output);
        m_output = nullptr;
        break;
      }
    }
  }
  FragmentStatistics::~FragmentStatistics() {
    for (auto &frame : m_frames) {
      if (frame.fragments) vkDestroyQueryPool(m_device, frame.fragments, nullptr);
      if (frame.samples) vkDestroyQueryPool(m_device, frame.samples, nullptr);
    }
    // Do not read pending slots here: the owner need not have submitted them.
    if (m_output) std::fclose(m_output);
  }
  void FragmentStatistics::selectFrame(uint32_t slot) {
    m_slot = slot;
    if (!enabled()) return;
    auto &frame = m_frames.at(slot);
    if (!frame.used) return;
    struct Result { uint64_t value = 0, available = 0; };
    // Vulkan returns counters in ascending statistic-bit order, then availability.
    struct PipelineResult { uint64_t primitives = 0, vertices = 0, fragments = 0, available = 0; };
    std::array<PipelineResult, capacity> fragments{};
    std::array<Result, capacity> samples{};
    const auto flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    const auto a = vkGetQueryPoolResults(m_device, frame.fragments, 0, frame.used,
        sizeof(PipelineResult) * frame.used, fragments.data(), sizeof(PipelineResult), flags);
    const auto b = vkGetQueryPoolResults(m_device, frame.samples, 0, frame.used,
        sizeof(Result) * frame.used, samples.data(), sizeof(Result), flags);
    // No WAIT_BIT: instrumentation never adds a GPU completion wait.
    for (uint32_t i = 0; i < frame.used; ++i) {
      const bool ready = a == VK_SUCCESS && b == VK_SUCCESS && fragments[i].available && samples[i].available;
      std::fprintf(m_output, "%llu,%llu,%u,%u,%u,%u,%llu,%llu,%u,%u,%llu,%llu\n",
          static_cast<unsigned long long>(frame.serial), static_cast<unsigned long long>(frame.bakeHash),
          frame.extent.width, frame.extent.height, frame.sampleCount, frame.categories[i],
          static_cast<unsigned long long>(ready ? fragments[i].fragments : 0),
          static_cast<unsigned long long>(ready ? samples[i].value : 0), unsigned(ready), frame.omitted,
          static_cast<unsigned long long>(ready ? fragments[i].primitives : 0),
          static_cast<unsigned long long>(ready ? fragments[i].vertices : 0));
    }
    std::fflush(m_output);
    frame.used = 0;
  }
  void FragmentStatistics::reset(VkCommandBuffer command, VkExtent2D extent,
                                 VkSampleCountFlagBits samples, uint64_t bakeHash) {
    if (!enabled()) return;
    auto &frame = m_frames[m_slot];
    frame.used = frame.omitted = 0;
    frame.active = false;
    frame.extent = extent;
    frame.sampleCount = uint32_t(samples);
    frame.bakeHash = bakeHash;
    frame.serial = ++m_serial;
    vkCmdResetQueryPool(command, frame.fragments, 0, capacity);
    vkCmdResetQueryPool(command, frame.samples, 0, capacity);
  }
  void FragmentStatistics::begin(VkCommandBuffer command, uint32_t category) {
    if (!enabled()) return;
    auto &frame = m_frames[m_slot];
    if (frame.used == capacity) { ++frame.omitted; return; }
    frame.categories[frame.used] = category;
    vkCmdBeginQuery(command, frame.fragments, frame.used, 0);
    vkCmdBeginQuery(command, frame.samples, frame.used, VK_QUERY_CONTROL_PRECISE_BIT);
    frame.active = true;
  }
  void FragmentStatistics::end(VkCommandBuffer command) {
    if (!enabled()) return;
    auto &frame = m_frames[m_slot];
    if (!frame.active) return;
    vkCmdEndQuery(command, frame.samples, frame.used);
    vkCmdEndQuery(command, frame.fragments, frame.used);
    ++frame.used;
    frame.active = false;
  }
}
