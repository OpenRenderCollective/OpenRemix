#include "forward/probes/partition_cells.h"
#include "forward/probes/regions.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace remix::forward::probes {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection site search superseded");
    }
    bool validPlane(const scene::VisibilityPartition::Node &node) {
      double squared = 0;
      for (uint32_t c = 0; c < 4; ++c) {
        if (!std::isfinite(node.plane[c]))
          return false;
        if (c < 3)
          squared += double(node.plane[c]) * node.plane[c];
      }
      return squared > 0;
    }
    // First solid along an axial segment, visiting near children first. A
    // missing hit returns range; malformed/cyclic walks refuse the candidate.
    std::optional<float> solidDistance(const scene::VisibilityPartition &partition, Vec3 origin,
                                       uint32_t axis, double sign, float range) {
      struct Segment {
        int32_t index;
        double first, last;
      };
      std::vector<Segment> stack{{0, 0, range}};
      size_t visited = 0;
      while (!stack.empty()) {
        if (++visited > partition.nodes.size() * 4 + 1024)
          return {};
        const auto entry = stack.back();
        stack.pop_back();
        if (entry.index < 0) {
          const auto leaf = uint64_t(-1 - int64_t(entry.index));
          if (leaf >= partition.leafClusters.size())
            return {};
          const auto cluster = partition.leafClusters[leaf];
          if (cluster < 0)
            return float(entry.first);
          if (uint32_t(cluster) >= partition.clusterCount)
            return {};
          continue;
        }
        if (size_t(entry.index) >= partition.nodes.size())
          return {};
        const auto &node = partition.nodes[entry.index];
        if (!validPlane(node))
          return {};
        double start = -double(node.plane[3]);
        for (uint32_t c = 0; c < 3; ++c)
          start += double(origin[c]) * node.plane[c];
        const double slope = sign * node.plane[axis];
        if (slope == 0) {
          stack.push_back({node.children[start >= 0 ? 0 : 1], entry.first, entry.last});
          continue;
        }
        const double split = -start / slope;
        if (split <= entry.first || split >= entry.last) {
          const bool front = start + std::midpoint(entry.first, entry.last) * slope >= 0;
          stack.push_back({node.children[front ? 0 : 1], entry.first, entry.last});
        } else {
          const uint32_t nearSide = slope > 0 ? 1 : 0;
          stack.push_back({node.children[1 - nearSide], split, entry.last});
          stack.push_back({node.children[nearSide], entry.first, split});
        }
      }
      return range;
    }
    std::optional<RegionSite> enclosure(const scene::VisibilityPartition &partition, Vec3 position,
                                         float clearance, float enclosureRange) {
      RegionSite site{position, enclosureRange, 0};
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const auto a = solidDistance(partition, position, axis, 1, enclosureRange);
        const auto b = solidDistance(partition, position, axis, -1, enclosureRange);
        if (!a || !b)
          return {};
        site.clearance = std::min({site.clearance, *a, *b});
        site.enclosedAxes += *a < enclosureRange && *b < enclosureRange;
      }
      return site.clearance >= clearance ? std::optional{site} : std::nullopt;
    }
    std::vector<detail::Box> emptyCells(const scene::VisibilityPartition &partition, const Bounds &limit,
                                        std::stop_token cancelled) {
      struct Entry {
        int32_t index;
        bool completed;
        detail::Box bounds;
      };
      detail::Box initial;
      for (uint32_t c = 0; c < 3; ++c) {
        initial.minimum[c] = limit.minimum[c];
        initial.maximum[c] = limit.maximum[c];
      }
      std::vector<Entry> stack{{0, false, initial}};
      std::vector<bool> active(partition.nodes.size());
      std::vector<detail::Box> cells;
      size_t visited = 0;
      while (!stack.empty()) {
        if (++visited > partition.nodes.size() * 4 + 1024)
          return {};
        if ((visited & 255u) == 0)
          check(cancelled);
        const auto entry = stack.back();
        stack.pop_back();
        if (entry.index < 0) {
          const auto leaf = uint64_t(-1 - int64_t(entry.index));
          if (leaf >= partition.leafClusters.size())
            return {};
          const auto cluster = partition.leafClusters[leaf];
          if (cluster >= 0) {
            if (uint32_t(cluster) >= partition.clusterCount || cells.size() == 65536)
              return {};
            cells.push_back(entry.bounds);
          }
          continue;
        }
        if (size_t(entry.index) >= partition.nodes.size())
          return {};
        if (entry.completed) {
          active[entry.index] = false;
          continue;
        }
        if (active[entry.index] || !validPlane(partition.nodes[entry.index]))
          return {};
        active[entry.index] = true;
        stack.push_back({entry.index, true, entry.bounds});
        for (uint32_t child = 0; child < 2; ++child) {
          auto bounds = entry.bounds;
          if (detail::clip(bounds, partition.nodes[entry.index].plane, child ? -1 : 1))
            stack.push_back({partition.nodes[entry.index].children[child], false, bounds});
        }
      }
      return cells;
    }
  } // namespace
  std::optional<RegionSite> regionSite(const scene::VisibilityPartition &partition, Vec3 position,
                                       float clearance, float enclosureRange,
                                       std::stop_token cancelled) {
    check(cancelled);
    if (!partition.closed || partition.nodes.empty() || !std::isfinite(clearance) ||
        !std::isfinite(enclosureRange) || clearance <= 0 || enclosureRange <= clearance)
      return {};
    Bounds safety;
    for (uint32_t c = 0; c < 3; ++c) {
      if (!std::isfinite(position[c]))
        return {};
      safety.minimum[c] = position[c] - clearance;
      safety.maximum[c] = position[c] + clearance;
    }
    safety.valid = true;
    RegionQuery query(partition, cancelled);
    if (!query.contains(safety))
      return {};
    return enclosure(partition, position, clearance, enclosureRange);
  }
  std::vector<RegionSite> regionSites(const scene::VisibilityPartition &partition, const Bounds &limit,
                                      float clearance, float enclosureRange, std::stop_token cancelled) {
    check(cancelled);
    if (!partition.closed || partition.nodes.empty() || !limit.valid || !std::isfinite(clearance) ||
        !std::isfinite(enclosureRange) || clearance <= 0 || enclosureRange <= clearance)
      return {};
    for (uint32_t c = 0; c < 3; ++c)
      if (!std::isfinite(limit.minimum[c]) || !std::isfinite(limit.maximum[c]) ||
          limit.minimum[c] >= limit.maximum[c])
        return {};
    const auto cells = emptyCells(partition, limit, cancelled);
    std::vector<RegionSite> sites;
    RegionQuery query(partition, cancelled);
    for (const auto &cell : cells) {
      check(cancelled);
      std::optional<Vec3> position;
      // Midpoint first, then quarter points. Oblique path bounds can overestimate
      // the leaf: never accept their midpoint without whole-clearance validation.
      constexpr float fraction[] = {.5f, .25f, .75f};
      for (float x : fraction) {
        for (float y : fraction) {
          for (float z : fraction) {
            const Vec3 candidate{float(std::lerp(cell.minimum[0], cell.maximum[0], x)),
                                 float(std::lerp(cell.minimum[1], cell.maximum[1], y)),
                                 float(std::lerp(cell.minimum[2], cell.maximum[2], z))};
            if (partition.cluster(candidate.data()) < 0)
              continue;
            Bounds safety;
            for (uint32_t c = 0; c < 3; ++c) {
              safety.minimum[c] = candidate[c] - clearance;
              safety.maximum[c] = candidate[c] + clearance;
            }
            safety.valid = true;
            if (query.contains(safety)) {
              position = candidate;
              break;
            }
          }
          if (position)
            break;
        }
        if (position)
          break;
      }
      if (!position)
        continue;
      if (const auto site = enclosure(partition, *position, clearance, enclosureRange))
        sites.push_back(*site);
    }
    // Cap the clearance preference so a cavern cannot starve every ordinary
    // room. Deterministic position ties make map reloads reproducible.
    std::sort(sites.begin(), sites.end(), [&](const auto &a, const auto &b) {
      const float ac = std::min(a.clearance, enclosureRange / 16),
                  bc = std::min(b.clearance, enclosureRange / 16);
      return std::tuple{-int(a.enclosedAxes), -ac, a.position} <
             std::tuple{-int(b.enclosedAxes), -bc, b.position};
    });
    if (sites.size() > 4096)
      sites.resize(4096);
    return sites;
  }
} // namespace remix::forward::probes
