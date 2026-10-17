#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace remix::forward::lightmap::curved {
  using Point = std::array<double, 3>;
  using Uv = std::array<double, 2>;
  struct Surface {
    std::vector<Point> positions; // metres, with original vertex identity preserved
    std::vector<Uv> authored;
    std::vector<uint32_t> corners; // indexed-corner order, like the source chart
  };
  inline Point difference(Point a, Point b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
  inline double dot(Point a, Point b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
  inline Point cross(Point a, Point b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  }
  double length(Point value);
  std::optional<std::vector<Uv>> harmonic(const Surface &surface, std::stop_token cancelled);
  // Open fans unfold around their common vertex using actual triangle angles
  // and edge lengths. Refuse disconnected, degenerate or overlapping fans.
  std::optional<std::vector<Uv>> unfoldFan(const Surface &surface, std::stop_token cancelled);
  std::vector<Uv> correctMetric(const Surface &surface, std::span<const Uv> coordinates,
                                std::stop_token cancelled);
} // namespace remix::forward::lightmap::curved
