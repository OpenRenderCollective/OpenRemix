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
                                                         std::stop_token cancelled,
                                                         const DirectPolicy &directLighting) {
    check(cancelled);
    const float spacing = grid.header.originSpacing[3], bias = grid.header.settings[0];
    if (grid.points.size() != grid.probes.size() || grid.points.size() > UINT32_MAX ||
        lights.size() >= (1u << 24) || !std::isfinite(spacing) || spacing <= 0 || !std::isfinite(bias) ||
        bias < 0 || !std::isfinite(cutoff) || cutoff <= 0 ||
        (!directLighting.lights.empty() && directLighting.lights.size() != lights.size()))
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
    struct Candidate {
      uint32_t light;
      size_t first, end;
      uint64_t count;
      double radius;
    };
    std::vector<Candidate> candidates;
    // A trilinear corner can be sqrt(3) cells from the receiver. Relocation
    // moves it at most .45 cells per axis; include the receiver's normal bias.
    const double halo = std::sqrt(3.) * 1.45 * spacing + bias;
    std::vector<double> probeHalos;
    if (adaptive(grid.header)) {
      // A fine-only record cannot contribute to a coarse stencil. Directory
      // origins retain the largest referencing/trace spacing, including shared
      // coarse corners and aligned records traced before further subdivision.
      // The finest spacing would be unsafe for those shared records.
      const auto origins = directoryOrigins(grid);
      probeHalos.reserve(origins.size());
      for (const auto &origin : origins)
        probeHalos.push_back(std::sqrt(3.) * 1.45 * origin.relocationSpacing +
                             double(bias) * origin.relocationSpacing / spacing);
    }
    const auto receiverReach = [&](double radius, uint32_t probe) {
      return radius + (probeHalos.empty() ? halo : probeHalos[probe]);
    };
    for (uint32_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex) {
      check(cancelled);
      const auto &light = lights[lightIndex];
      if (light.dynamic)
        continue;
      if (!directLighting.lights.empty() && !retainsVisibility(directLighting.lights[lightIndex]))
        continue; // Fully baked diffuse needs no per-light probe shadow mask.
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
      const auto end = std::upper_bound(
          first, sorted.end(), double(light.position[sweepAxis]) + reach,
          [&](double x, uint32_t probe) { return x < grid.points[probe].position[sweepAxis]; });
      uint64_t count = 0;
      for (auto entry = first; entry != end; ++entry) {
        const auto &position = grid.points[*entry].position;
        if ((result.candidates++ & 1023u) == 0)
          check(cancelled);
        double distanceSquared = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const double delta = double(position[axis]) - light.position[axis];
          distanceSquared += delta * delta;
        }
        const double localReach = receiverReach(radius, *entry);
        if (distanceSquared > localReach * localReach)
          continue;
        // A single light larger than the entire budget cannot be selected.
        // Stop counting it, but continue examining later, cheaper volumes.
        if (++count > maximumPairs)
          break;
      }
      if (count)
        candidates.push_back(
            {lightIndex, size_t(first - sorted.begin()), size_t(end - sorted.begin()), count, radius});
    }
    result.eligibleLights = uint32_t(candidates.size());
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
      return a.count < b.count || (a.count == b.count && a.light < b.light);
    });
    uint64_t selectedPairs = 0;
    for (const auto &candidate : candidates) {
      if (candidate.count > maximumPairs - selectedPairs)
        break;
      selectedPairs += candidate.count;
      ++result.selectedLights;
    }
    result.deferredLights = result.eligibleLights - result.selectedLights;
    if (result.eligibleLights && !result.selectedLights)
      return {};
    result.tasks.reserve(size_t(selectedPairs));
    for (uint32_t selected = 0; selected < result.selectedLights; ++selected) {
      const auto &candidate = candidates[selected];
      const auto &light = lights[candidate.light];
      for (size_t i = candidate.first; i < candidate.end; ++i) {
        if ((result.candidates++ & 1023u) == 0)
          check(cancelled);
        const auto &position = grid.points[sorted[i]].position;
        double distanceSquared = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const double delta = double(position[axis]) - light.position[axis];
          distanceSquared += delta * delta;
        }
        const double localReach = receiverReach(candidate.radius, sorted[i]);
        if (distanceSquared <= localReach * localReach)
          result.tasks.push_back({sorted[i], candidate.light, 0});
      }
    }
    check(cancelled);
    return result;
  }
} // namespace remix::forward::bake
