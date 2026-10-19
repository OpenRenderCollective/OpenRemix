#pragma once
#include <array>
#include <cstdint>
#include <cmath>

namespace remix::scene {
  // Rows of a rigid world-to-projection transform. Bounds are in this space;
  // capture positions and captured cubemap directions remain in world space.
  using ProjectionSpace = std::array<std::array<float, 4>, 3>;
  inline constexpr ProjectionSpace worldSpace{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
  inline std::array<float, 3> projectionDirection(const ProjectionSpace &space, std::array<float, 3> value) {
    std::array<float, 3> result{};
    for (uint32_t row = 0; row < 3; ++row)
      for (uint32_t c = 0; c < 3; ++c) result[row] += space[row][c] * value[c];
    return result;
  }
  inline std::array<float, 3> projectionPoint(const ProjectionSpace &space, std::array<float, 3> value) {
    auto result = projectionDirection(space, value);
    for (uint32_t c = 0; c < 3; ++c) result[c] += space[c][3];
    return result;
  }
  inline bool validProjectionSpace(const ProjectionSpace &space) {
    for (const auto &row : space)
      for (float value : row) if (!std::isfinite(value)) return false;
    for (uint32_t a = 0; a < 3; ++a)
      for (uint32_t b = 0; b < 3; ++b) {
        float product = 0;
        for (uint32_t c = 0; c < 3; ++c) product += space[a][c] * space[b][c];
        if (std::abs(product - (a == b ? 1.f : 0.f)) > 1e-4f) return false;
      }
    // Reject reflections as well as scaling/shear: the authoring frame is a rotation.
    const auto &a = space[0], &b = space[1], &c = space[2];
    const float determinant = a[0] * (b[1]*c[2] - b[2]*c[1]) -
                              a[1] * (b[0]*c[2] - b[2]*c[0]) +
                              a[2] * (b[0]*c[1] - b[1]*c[0]);
    return determinant > .999f;
  }
} // namespace remix::scene
