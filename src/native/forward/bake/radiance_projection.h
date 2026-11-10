#pragma once
#include "forward/bake/spherical_harmonics.h"
#include <algorithm>
#include <numbers>

namespace remix::forward::bake {
  // Matched incoming-radiance estimators. One uniform-sphere ray contributes
  // to both representations; no extra traces or receiver-direct NEE are added.
  class RadianceProjection {
  public:
    using Rgb = std::array<double, 3>;
    using Sh = std::array<Rgb, 9>;
    using Cube = std::array<Rgb, 6>; // +X, -X, +Y, -Y, +Z, -Z
    void add(Vec3 direction, Vec3 radiance) {
      const auto basis = irradianceBasis(direction);
      for (uint32_t j = 0; j < 9; ++j)
        for (uint32_t c = 0; c < 3; ++c) m_sh[j][c] += double(basis[j]) * radiance[c];
      for (uint32_t face = 0; face < 6; ++face) {
        const double cosine = std::max(0.f, direction[face / 2] * (face % 2 ? -1.f : 1.f));
        for (uint32_t c = 0; c < 3; ++c) m_cube[face][c] += cosine * radiance[c];
      }
      ++m_samples;
    }
    Sh sphericalHarmonics() const {
      auto result = m_sh;
      for (uint32_t j = 0; j < 9; ++j) {
        const double convolution = j == 0 ? std::numbers::pi : j < 4 ? 2 * std::numbers::pi / 3
                                                                      : std::numbers::pi / 4;
        for (auto &channel : result[j]) channel *= normalization() * convolution;
      }
      return result;
    }
    Cube ambientCube() const {
      auto result = m_cube;
      for (auto &face : result) for (auto &channel : face) channel *= normalization();
      return result;
    }
  private:
    double normalization() const { return m_samples ? 4 * std::numbers::pi / m_samples : 0; }
    Sh m_sh{};
    Cube m_cube{};
    uint32_t m_samples = 0;
  };
}
