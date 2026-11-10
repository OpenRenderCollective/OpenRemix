#include "forward/bake/probe_connection_audit.h"
#include <cmath>

namespace remix::forward::bake {
  ConnectionAudit probeConnections(const scene::BakeScene &source, const Grid &grid,
      const ProbeStencil &stencil, std::span<const Grid::Origin> origins, std::stop_token cancelled, uint32_t excludedDraw) {
    if (!(source.sceneScale > 0) || !std::isfinite(source.sceneScale))
      throw std::invalid_argument("Invalid probe connection scene scale");
    ConnectionAudit result;
    std::array<float, 16> lengths{};
    std::vector<ReceiverRay> rays;
    std::vector<uint32_t> slots;
    // Geometry-only evidence, not alpha/transmission visibility. Keep the
    // exact biased endpoint, excluding both endpoints by 0.1 mm.
    const float tolerance = .0001f / source.sceneScale;
    for (uint32_t c = 0; c < stencil.count; ++c) {
      if (stencil.weights[c] <= 0) continue;
      const auto index = stencil.indices[c];
      if (index >= origins.size()) throw std::invalid_argument("Invalid probe connection origin");
      auto &position = result.positions[c];
      position = origins[index].position;
      if (uint32_t(grid.header.settings[2]) & 1u)
        for (uint32_t axis = 0; axis < 3; ++axis) position[axis] += grid.probes.at(index).irradiance[axis][3];
      Vec3 direction;
      for (uint32_t axis = 0; axis < 3; ++axis)
        direction[axis] = stencil.receiverPositions[c / 8][axis] - position[axis];
      const float distance = std::sqrt(dot(direction, direction));
      if (!std::isfinite(distance) || distance <= 2 * tolerance) continue;
      ReceiverRay ray;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        ray.direction[axis] = direction[axis] / distance;
        ray.origin[axis] = position[axis] + ray.direction[axis] * tolerance;
      }
      rays.push_back(ray); slots.push_back(c);
      lengths[c] = distance - 2 * tolerance;
      result.tested[c] = true;
    }
    if (!rays.empty()) {
      const auto hits = pickProbeReceivers(source, rays, cancelled, excludedDraw);
      for (size_t c = 0; c < slots.size(); ++c)
        if (hits[c] && hits[c]->distance < lengths[slots[c]]) result.blockers[slots[c]] = hits[c];
    }
    return result;
  }
}
