#pragma once
#include "forward/bake/receiver_projection.h"
#include "forward/bake/transport.h"
#include "forward/resources.h"
namespace remix::forward::bake {
  // Immutable capture layout and resources retained by the published volume.
  // Only the render thread records writes, and publication waits for all fences.
  class SunDepth {
  public:
    static std::shared_ptr<SunDepth> prepare(VkPhysicalDevice physical, VkDevice device,
        const scene::BakeScene &source, const lightmap::Layout &receivers, std::stop_token cancelled);
    float distance() const { return m_header.sampling[2] - m_header.sampling[1]; }
    uint32_t tasks() const { return uint32_t(m_layout.tiles.size()) * m_extent * m_extent; }
    std::vector<Transport::DepthRay> batch(uint32_t first, uint32_t count) const;
    VkDescriptorBufferInfo table() const { return m_table.descriptor(); }
    VkDescriptorBufferInfo values() const { return m_values.descriptor(); }
    uint64_t bytes() const { return m_table.allocationBytes + m_values.allocationBytes; }
  private:
    SunDepthLayout m_layout;
    SunDepthHeader m_header;
    std::array<Vec3, 3> m_basis;
    uint32_t m_extent = 0;
    Buffer m_table, m_values;
  };
}
