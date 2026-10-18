#pragma once
#include <array>
#include <cstdint>

namespace remix::forward::lightmap {
  // Intersection of one actual surface triangle with a luxel square. UVs are
  // chart-local luxels, not atlas coordinates. A clipped triangle has at most
  // seven vertices. No heap allocation or extrapolated surface donors.
  class ReceiverFootprint {
  public:
    using Uv = std::array<float, 2>;
    ReceiverFootprint(const std::array<Uv, 3> &triangle, Uv centre);
    double area() const { return m_twiceArea * .5; }
    bool empty() const { return m_twiceArea < 1e-12; }
    Uv centroid() const;
    // Two independent uniform variates in [0,1). Fan triangles are selected
    // proportional to area, then sampled uniformly, including partial luxels.
    // Map the returned UV back through the original triangle's barycentrics.
    Uv sample(double u, double v) const;

  private:
    std::array<Uv, 7> m_vertices{};
    uint32_t m_count = 0;
    double m_twiceArea = 0;
  };
} // namespace remix::forward::lightmap
