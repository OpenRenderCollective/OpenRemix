#pragma once
#include "forward/lightmap/layout.h"
#include "forward/lightmap/receiver_footprint.h"

namespace remix::forward::lightmap {
  // Borrowed immutable geometry/layout must outlive this sampler. One caller
  // owns its reusable scratch: no full-map array of four expanded origins.
  class ReceiverSampler {
  public:
    ReceiverSampler(const scene::BakeScene &source, const Layout &layout);
    std::array<bake::Point, 4> sample(uint32_t point);
    uint64_t memoryBytes() const;
    uint32_t fallbackCount() const { return m_fallbackCount; }

  private:
    struct Triangle {
      std::array<ReceiverFootprint::Uv, 3> uv;
      std::array<Vec3, 3> positions, normals;
      ReceiverFootprint::Uv minimum, maximum;
      double area;
    };
    struct Hit {
      uint32_t triangle;
      ReceiverFootprint footprint;
    };
    const scene::BakeScene &m_source;
    const Layout &m_layout;
    uint32_t m_chart = unowned;
    uint32_t m_fallbackCount = 0;
    std::vector<Triangle> m_triangles;
    std::vector<Hit> m_hits;
    void prepare(uint32_t chart);
  };
} // namespace remix::forward::lightmap
