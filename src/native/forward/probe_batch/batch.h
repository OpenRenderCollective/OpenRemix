#pragma once
#include "forward/resources.h"
#include <memory>
#include <span>
#include <vector>

namespace remix::forward::bake { struct Volume; }
namespace remix::forward::probe_batch {
  struct Resources;
  // Immutable GPU-derived probe-major vec4 masks. Negative components preserve
  // unknown coverage; neither masks nor directional records are quantized.
  class Field {
  public:
    ~Field();
    VkDescriptorBufferInfo descriptor() const { return m_values.descriptor(); }
    uint32_t probes() const { return m_probes; }
    uint32_t groups() const { return m_groups; }
    std::span<const uint32_t> lights() const { return m_lights; }
    uint64_t bufferBytes() const { return m_bufferBytes; }

  private:
    friend class Cache;
    Field() = default;
    std::shared_ptr<Resources> m_resources;
    std::shared_ptr<const bake::Volume> m_source;
    std::vector<uint32_t> m_lights;
    uint32_t m_probes = 0, m_groups = 0;
    Buffer m_parameters, m_values;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkEvent m_completed = VK_NULL_HANDLE;
    uint64_t m_bufferBytes = 0;
  };
  // Render-thread state, prepared before activation. The runtime's pipeline
  // service and Vulkan device outlive this cache and every submitted Field.
  class Cache {
  public:
    static constexpr uint32_t maximumLights = 64;
    static constexpr uint64_t maximumFieldBytes = 16ull << 20;
    // Budget is addressable field payload; resident accounting uses the buffers'
    // allocationBytes, including device allocation alignment and parameters.
    Cache(VkPhysicalDevice physical, VkDevice device, uint64_t byteBudget = maximumFieldBytes);
    // Run in the existing pipeline preparation worker before recording. A cache
    // not yet prepared declines the optional field instead of compiling mid-frame.
    void prepare();
    // Caller retains the returned owner through command cancellation or its
    // submission fence. Commands and later sampling use the same graphics queue.
    // Reuse requires a device-set completion event, never recording alone.
    std::shared_ptr<const Field> record(VkCommandBuffer command,
        std::shared_ptr<const bake::Volume> source, std::span<const uint32_t> capturedLights);
    void reset() { m_fields.clear(); }
    // Includes fields retained by submitted consumers after source/selection changes.
    // Source bake buffers remain accounted by the bake controller.
    uint64_t bufferBytes() const;
    uint64_t peakBufferBytes() const;
    // Recorded producers include abandoned commands; only a completion event
    // permits reuse. These counters diagnose work, not completed GPU dispatches.
    uint64_t recordedGathers() const { return m_recordedGathers; }
    uint64_t completedReuses() const { return m_completedReuses; }

  private:
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    uint64_t m_byteBudget, m_storageRange;
    std::shared_ptr<Resources> m_resources;
    std::vector<std::shared_ptr<Field>> m_fields;
    uint64_t m_recordedGathers = 0, m_completedReuses = 0;
  };
}
