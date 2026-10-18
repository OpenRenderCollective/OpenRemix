#include "forward/bake/probe_visibility_plan.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Probe visibility planning cancelled");
    }
  } // namespace
  std::optional<ProbeVisibilityPlan> planProbeVisibility(const Grid &grid,
                                                         std::span<const scene::SphereLight> lights,
                                                         float cutoff, uint32_t maximumPairs,
                                                         std::stop_token cancelled) {
    check(cancelled);
    const float spacing = grid.header.originSpacing[3], bias = grid.header.settings[0];
    if (grid.points.size() != grid.probes.size() || grid.points.size() > UINT32_MAX ||
        lights.size() >= (1u << 24) || !std::isfinite(spacing) || spacing <= 0 || !std::isfinite(bias) ||
        bias < 0 || !std::isfinite(cutoff) || cutoff <= 0)
      throw std::invalid_argument("Invalid local probe visibility planning inputs");
    std::vector<uint32_t> sorted;
    Bounds bounds;
    sorted.reserve(grid.probes.size());
    for (uint32_t i = 0; i < grid.probes.size(); ++i) {
      if ((i & 1023u) == 0)
        check(cancelled);
      if (!std::isfinite(grid.probes[i].statistics[3]))
        throw std::invalid_argument("Non-finite probe usability");
      if (grid.probes[i].statistics[3] <= .5f)
        continue;
      for (uint32_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(grid.points[i].position[axis]))
          throw std::invalid_argument("Non-finite local probe visibility position");
      sorted.push_back(i);
      bounds.include({grid.points[i].position[0], grid.points[i].position[1], grid.points[i].position[2]});
    }
    uint32_t sweepAxis = 0;
    for (uint32_t axis = 1; axis < 3; ++axis)
      if (bounds.maximum[axis] - double(bounds.minimum[axis]) >
          bounds.maximum[sweepAxis] - double(bounds.minimum[sweepAxis]))
        sweepAxis = axis;
    std::sort(sorted.begin(), sorted.end(), [&](uint32_t a, uint32_t b) {
      const float x = grid.points[a].position[sweepAxis], y = grid.points[b].position[sweepAxis];
      return x < y || (x == y && a < b);
    });
    ProbeVisibilityPlan result;
    // A trilinear corner can be sqrt(3) cells from the receiver. Relocation
    // moves it at most .45 cells per axis; include the receiver's normal bias.
    const double halo = std::sqrt(3.) * 1.45 * spacing + bias;
    for (uint32_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex) {
      check(cancelled);
      const auto &light = lights[lightIndex];
      if (light.dynamic)
        continue;
      for (float value : light.position)
        if (!std::isfinite(value))
          throw std::invalid_argument("Non-finite local light position");
      const float radius = influenceRadius(light, cutoff);
      if (!std::isfinite(radius))
        throw std::invalid_argument("Invalid local light influence radius");
      if (radius <= 0)
        continue;
      const double reach = double(radius) + halo;
      const auto first = std::lower_bound(
          sorted.begin(), sorted.end(), double(light.position[sweepAxis]) - reach,
          [&](uint32_t probe, double x) { return grid.points[probe].position[sweepAxis] < x; });
      for (auto entry = first; entry != sorted.end(); ++entry) {
        const auto &position = grid.points[*entry].position;
        if (position[sweepAxis] > double(light.position[sweepAxis]) + reach)
          break;
        if ((result.candidates++ & 1023u) == 0)
          check(cancelled);
        double distanceSquared = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const double delta = double(position[axis]) - light.position[axis];
          distanceSquared += delta * delta;
        }
        if (distanceSquared > reach * reach)
          continue;
        if (result.tasks.size() == maximumPairs)
          return {};
        result.tasks.push_back({*entry, lightIndex, 0});
      }
    }
    check(cancelled);
    return result;
  }
} // namespace remix::forward::bake
