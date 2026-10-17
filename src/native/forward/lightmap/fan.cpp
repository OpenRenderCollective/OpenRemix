#include "forward/lightmap/surface_metric.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace remix::forward::lightmap::curved {
  std::optional<std::vector<Uv>> unfoldFan(const Surface &surface, std::stop_token cancelled) {
    if (cancelled.stop_requested())
      throw std::runtime_error("Fan surface unfolding superseded");
    const auto count = surface.positions.size();
    if (count < 3 || surface.corners.size() != (count - 2) * 3)
      return std::nullopt;
    uint32_t root = ~0u;
    for (uint32_t candidate : std::span(surface.corners).first(3)) {
      bool common = true;
      for (size_t i = 0; i < surface.corners.size(); i += 3) {
        if ((i & 1023u) == 0 && cancelled.stop_requested())
          throw std::runtime_error("Fan surface unfolding superseded");
        common &= surface.corners[i] == candidate || surface.corners[i + 1] == candidate ||
                  surface.corners[i + 2] == candidate;
      }
      if (common)
        root = std::min(root, candidate);
    }
    if (root >= count)
      return std::nullopt;
    std::vector<uint32_t> next(count, ~0u), incoming(count);
    for (size_t i = 0; i < surface.corners.size(); i += 3) {
      if ((i & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Fan surface unfolding superseded");
      uint32_t corner = 0;
      while (surface.corners[i + corner] != root)
        ++corner;
      const auto a = surface.corners[i + (corner + 1) % 3], b = surface.corners[i + (corner + 2) % 3];
      if (a >= count || b >= count || a == b || a == root || b == root || next[a] != ~0u ||
          ++incoming[b] > 1)
        return std::nullopt;
      next[a] = b;
    }
    uint32_t start = ~0u;
    for (uint32_t i = 0; i < count; ++i)
      if (i != root && !incoming[i]) {
        if (start != ~0u)
          return std::nullopt;
        start = i;
      }
    if (start >= count)
      return std::nullopt;
    std::vector<Uv> uv(count);
    std::vector<uint8_t> visited(count);
    auto previous = difference(surface.positions[start], surface.positions[root]);
    double angle = 0;
    uint32_t reached = 0;
    for (auto current = start; current != ~0u; current = next[current]) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Fan surface unfolding superseded");
      if (visited[current])
        return std::nullopt;
      visited[current] = 1;
      ++reached;
      const auto edge = difference(surface.positions[current], surface.positions[root]);
      const double radius = length(edge);
      if (!(radius > 0) || !std::isfinite(radius))
        return std::nullopt;
      if (current != start) {
        const double area = length(cross(previous, edge));
        if (!(area > 1e-20))
          return std::nullopt;
        angle += std::atan2(area, dot(previous, edge));
      }
      // All triangles share the origin. A turn of 2 pi would overlap them.
      if (!std::isfinite(angle) || angle >= 2 * std::numbers::pi)
        return std::nullopt;
      uv[current] = {radius * std::cos(angle), radius * std::sin(angle)};
      previous = edge;
    }
    return reached == count - 1 ? std::optional(std::move(uv)) : std::nullopt;
  }
} // namespace remix::forward::lightmap::curved
