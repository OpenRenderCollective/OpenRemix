#pragma once
#include "forward/resources.h"
#include <memory>
#include <span>
#include <vector>

namespace remix::forward::depth_batch {
  struct Source {
    const Buffer *vertices = nullptr, *indices = nullptr;
    uint32_t stride = 0, firstIndex = 0, indexCount = 0;
    int32_t vertexOffset = 0;
    uint32_t materialIndex = 0;
    bool visible = false;
    // Keeps immutable source buffers alive, including derived lightmap meshes.
    std::shared_ptr<const void> owner;
  };
  // One instance per fenced frame slot. prepare() may rewrite its buffers only
  // after that slot has completed. No source transform is baked into positions.
  class Batch {
  public:
    static constexpr uint64_t maximumBytes = 32ull << 20;
    Batch(VkPhysicalDevice physical, VkDevice device, uint64_t budget = maximumBytes);
    bool prepare(std::span<const Source> sources);
    const Buffer &positions() const { return *m_positions; }
    const Buffer &materials() const { return *m_materials; }
    const Buffer &indices() const { return *m_indices; }
    uint32_t indexCount() const { return m_indexCount; }
    uint32_t visibleSources() const { return m_visibleSources; }
    uint64_t bytes() const;

  private:
    struct Range { uint32_t vertex, vertices, index, indices; };
    bool rebuild(std::span<const Source> sources);
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    uint64_t m_budget;
    std::unique_ptr<Buffer> m_positions, m_materials, m_indices;
    std::vector<Source> m_sources, m_rejected;
    std::vector<Range> m_ranges;
    std::vector<uint32_t> m_allIndices, m_visibleIndices;
    uint32_t m_indexCount = 0, m_visibleSources = 0;
  };
} // namespace remix::forward::depth_batch
