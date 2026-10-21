#include "forward/bake/probe_visibility_plan.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Probe visibility planning cancelled");
    }
    struct Candidate {
      uint32_t light;
      size_t first, end;
      uint64_t count;
      double radius;
    };
    std::vector<size_t> selectVolumes(std::span<const Candidate> candidates,
                                      std::span<const scene::SphereLight> lights,
                                      uint32_t maximumPairs, bool completeStyleGroups,
                                      std::stop_token cancelled) {
      if (!completeStyleGroups || std::none_of(candidates.begin(), candidates.end(),
            [&](const Candidate &candidate) { return lights[candidate.light].style != 0; })) {
        uint64_t pairs = 0;
        std::vector<size_t> selected;
        for (size_t i = 0; i < candidates.size(); ++i) {
          if ((i & 1023u) == 0) check(cancelled);
          if (candidates[i].count > maximumPairs - pairs) break;
          pairs += candidates[i].count;
          selected.push_back(i);
        }
        return selected;
      }
      struct Group {
        std::vector<size_t> members;
        uint64_t pairs = 0;
        uint32_t firstLight = UINT32_MAX;
        bool selected = false;
      };
      std::vector<Group> groups;
      std::map<uint32_t, size_t> styles;
      for (size_t i = 0; i < candidates.size(); ++i) {
        if ((i & 1023u) == 0) check(cancelled);
        const auto &candidate = candidates[i];
        const uint32_t style = completeStyleGroups ? lights[candidate.light].style : 0;
        size_t group = groups.size();
        if (style) group = styles.try_emplace(style, group).first->second;
        if (group == groups.size()) groups.emplace_back();
        groups[group].members.push_back(i);
        groups[group].pairs += candidate.count;
        groups[group].firstLight = std::min(groups[group].firstLight, candidate.light);
      }
      // A controllable component can only replace runtime lights when every
      // member has a complete volume. Rank cost per light, not total group cost:
      // otherwise a large useful group is always considered last and stranded.
      std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        const double x = double(a.pairs) / a.members.size(), y = double(b.pairs) / b.members.size();
        return x != y ? x < y : a.firstLight < b.firstLight;
      });
      uint64_t pairs = 0;
      std::vector<size_t> chosen;
      for (size_t i = 0; i < groups.size(); ++i) {
        check(cancelled);
        auto &group = groups[i];
        if (group.pairs > maximumPairs) continue;
        if (group.pairs > maximumPairs - pairs) {
          // Repair a nearly fitting component by trading the most expensive
          // selected suffix for it, only if that retires strictly more lights.
          // Stop after losing its member count: this bounds repair work by the
          // number of captured lights, rather than solving a large knapsack.
          uint64_t released = 0;
          size_t lost = 0, remove = 0;
          while (remove < chosen.size()) {
            if ((remove & 1023u) == 0) check(cancelled);
            const auto &previous = groups[chosen[chosen.size() - 1 - remove]];
            lost += previous.members.size();
            if (lost >= group.members.size()) break;
            released += previous.pairs;
            ++remove;
            if (group.pairs <= maximumPairs - pairs + released) break;
          }
          if (lost >= group.members.size() || group.pairs > maximumPairs - pairs + released) continue;
          for (size_t j = 0; j < remove; ++j) {
            groups[chosen.back()].selected = false;
            chosen.pop_back();
          }
          pairs -= released;
        }
        pairs += group.pairs;
        group.selected = true;
        chosen.push_back(i);
      }
      // An eviction can leave room for an earlier skipped, cheaper unit.
      for (auto &group : groups) {
        check(cancelled);
        if (group.selected || group.pairs > maximumPairs - pairs) continue;
        group.selected = true;
        pairs += group.pairs;
      }
      std::vector<size_t> selected;
      for (const auto &group : groups)
        if (group.selected) selected.insert(selected.end(), group.members.begin(), group.members.end());
      // Preserve existing captured-cost order for transport and packing.
      std::sort(selected.begin(), selected.end());
      return selected;
    }
  } // namespace
  std::optional<ProbeVisibilityPlan> planProbeVisibility(const Grid &grid,
                                                         std::span<const scene::SphereLight> lights,
                                                         float cutoff, uint32_t maximumPairs,
                                                         std::stop_token cancelled,
                                                         const DirectPolicy &directLighting,
                                                         bool completeStyleGroups) {
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
    const auto selected = selectVolumes(candidates, lights, maximumPairs, completeStyleGroups, cancelled);
    uint64_t selectedPairs = 0;
    for (size_t index : selected) selectedPairs += candidates[index].count;
    result.selectedLights = uint32_t(selected.size());
    result.deferredLights = result.eligibleLights - result.selectedLights;
    if (result.eligibleLights && !result.selectedLights)
      return {};
    result.tasks.reserve(size_t(selectedPairs));
    for (size_t index : selected) {
      const auto &candidate = candidates[index];
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
