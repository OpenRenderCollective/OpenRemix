#include "forward/probes/selection.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace remix::forward::probes {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection coverage selection superseded");
    }
    double volume(const Bounds &box) {
      double result = 1;
      for (uint32_t c = 0; c < 3; ++c) {
        const double extent = double(box.maximum[c]) - box.minimum[c];
        if (!std::isfinite(extent) || extent <= 0)
          return 0;
        result *= extent;
      }
      return box.valid ? result : 0;
    }
    // Remove an intersection as up to six disjoint slabs. Keeping these pieces
    // gives exact union accounting without adding overlap twice or voxelizing.
    void subtract(const Bounds &box, const Bounds &cut, std::vector<Bounds> &pieces) {
      Bounds intersection = box;
      for (uint32_t c = 0; c < 3; ++c) {
        intersection.minimum[c] = std::max(box.minimum[c], cut.minimum[c]);
        intersection.maximum[c] = std::min(box.maximum[c], cut.maximum[c]);
      }
      if (!volume(intersection)) {
        pieces.push_back(box);
        return;
      }
      auto remaining = box;
      for (uint32_t c = 0; c < 3; ++c) {
        if (remaining.minimum[c] < intersection.minimum[c]) {
          auto slab = remaining;
          slab.maximum[c] = intersection.minimum[c];
          pieces.push_back(slab);
          remaining.minimum[c] = intersection.minimum[c];
        }
        if (remaining.maximum[c] > intersection.maximum[c]) {
          auto slab = remaining;
          slab.minimum[c] = intersection.maximum[c];
          pieces.push_back(slab);
          remaining.maximum[c] = intersection.maximum[c];
        }
      }
    }
  } // namespace
  std::vector<size_t> spreadSites(std::span<const RegionSite> sites, uint32_t budget, float clearanceCap,
                                  std::stop_token cancelled, std::span<const size_t> initial) {
    check(cancelled);
    if (!std::isfinite(clearanceCap) || clearanceCap <= 0)
      throw std::invalid_argument("Invalid reflection site clearance cap");
    std::vector<size_t> result;
    std::vector<bool> used(sites.size());
    std::vector<double> distance(sites.size(), std::numeric_limits<double>::infinity());
    auto include = [&](size_t site) {
      if (site >= sites.size() || used[site]) throw std::invalid_argument("Invalid initial reflection site");
      used[site] = true;
      result.push_back(site);
      for (size_t i = 0; i < sites.size(); ++i) {
        double squared = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          const double delta = double(sites[i].position[c]) - sites[site].position[c];
          squared += delta * delta;
        }
        distance[i] = std::min(distance[i], squared);
      }
    };
    if (initial.size() > budget) throw std::invalid_argument("Initial reflection sites exceed fitting budget");
    for (const auto site : initial) { check(cancelled); include(site); }
    auto priority = [&](size_t i) {
      return std::tuple{sites[i].enclosedAxes, distance[i] * std::min(sites[i].clearance, clearanceCap)};
    };
    while (result.size() < std::min(size_t(budget), sites.size())) {
      check(cancelled);
      size_t best = sites.size();
      for (size_t i = 0; i < sites.size(); ++i)
        if (!used[i] && (best == sites.size() || priority(i) > priority(best)))
          best = i;
      if (best == sites.size())
        break;
      include(best);
    }
    // Fit in original priority order so equal-coverage ties remain predictable.
    std::sort(result.begin(), result.end());
    return result;
  }
  std::vector<size_t> selectRegions(std::span<const FittedRegion> regions, uint32_t count,
                                    std::stop_token cancelled, std::span<const size_t> initial) {
    check(cancelled);
    const size_t limit = std::min(size_t(count), regions.size());
    if (initial.size() > count || initial.size() > limit)
      throw std::invalid_argument("Initial reflection regions exceed selection budget");
    std::vector<bool> selected(regions.size());
    for (const auto index : initial) {
      if (index >= regions.size() || selected[index])
        throw std::invalid_argument("Invalid initial reflection region");
      selected[index] = true;
    }
    std::vector<std::vector<Bounds>> uncovered;
    for (const auto &region : regions)
      uncovered.push_back({region.bounds});
    for (const auto index : initial) {
      uncovered[index].clear();
      for (auto &pieces : uncovered) {
        check(cancelled);
        std::vector<Bounds> remaining;
        for (const auto &piece : pieces) subtract(piece, regions[index].bounds, remaining);
        pieces = std::move(remaining);
      }
    }
    std::vector<size_t> result(initial.begin(), initial.end());
    while (result.size() < limit) {
      check(cancelled);
      size_t best = regions.size();
      double bestScore = 0;
      for (size_t i = 0; i < regions.size(); ++i) {
        if (selected[i]) continue;
        double gain = 0;
        for (const auto &piece : uncovered[i])
          gain += volume(piece);
        // Match the existing 95% duplicate tolerance, now against the union of
        // selected boxes. A large enclosing region can still add useful space.
        if (gain <= .05 * volume(regions[i].bounds))
          continue;
        const double score = gain * std::clamp(regions[i].quality, .05f, 1.f);
        // Equivalent fits can differ slightly after floating-point face growth.
        // Keep input/authored preference unless the score meaningfully
        // improves.
        if (best == regions.size() || regions[i].site.enclosedAxes > regions[best].site.enclosedAxes ||
            (regions[i].site.enclosedAxes == regions[best].site.enclosedAxes &&
             score > bestScore * 1.00001)) {
          best = i;
          bestScore = score;
        }
      }
      if (best == regions.size())
        break;
      result.push_back(best);
      selected[best] = true;
      uncovered[best].clear();
      for (auto &pieces : uncovered) {
        check(cancelled);
        std::vector<Bounds> remaining;
        for (const auto &piece : pieces)
          subtract(piece, regions[best].bounds, remaining);
        pieces = std::move(remaining);
      }
    }
    return result;
  }
  std::vector<size_t> selectSupportedRegions(std::span<const FittedRegion> regions,
      std::span<const std::vector<uint32_t>> footprints, std::span<const double> areas,
      uint32_t count, std::stop_token cancelled) {
    check(cancelled);
    if (regions.size() > 512 || footprints.size() != regions.size() || count > 64 || areas.size() > 8192)
      throw std::invalid_argument("Invalid receiver-supported selection dimensions");
    for (const auto &region : regions)
      if (!std::isfinite(region.quality))
        throw std::invalid_argument("Invalid reflection region quality");
    for (const auto area : areas)
      if (!std::isfinite(area) || area < 0)
        throw std::invalid_argument("Invalid receiver surface area");
    std::vector<std::vector<uint32_t>> normalized(footprints.begin(), footprints.end());
    for (auto &footprint : normalized) {
      std::sort(footprint.begin(), footprint.end());
      if (std::adjacent_find(footprint.begin(), footprint.end()) != footprint.end() ||
          (!footprint.empty() && footprint.back() >= areas.size()))
        throw std::invalid_argument("Invalid receiver footprint");
    }
    std::vector<bool> used(regions.size()), covered(areas.size());
    std::vector<size_t> result;
    while (result.size() < std::min(size_t(count), regions.size())) {
      check(cancelled);
      size_t best = regions.size();
      double bestScore = 0;
      for (size_t i = 0; i < regions.size(); ++i) {
        if (used[i]) continue;
        double gain = 0;
        for (const auto receiver : normalized[i])
          if (!covered[receiver]) gain += areas[receiver];
        if (!(gain > 0)) continue;
        const double score = gain * std::clamp(regions[i].quality, .05f, 1.f);
        if (best == regions.size() || regions[i].site.enclosedAxes > regions[best].site.enclosedAxes ||
            (regions[i].site.enclosedAxes == regions[best].site.enclosedAxes && score > bestScore * 1.00001)) {
          best = i;
          bestScore = score;
        }
      }
      if (best == regions.size()) break;
      result.push_back(best);
      used[best] = true;
      for (const auto receiver : normalized[best]) covered[receiver] = true;
    }
    if (result.size() < std::min(size_t(count), regions.size()))
      result = selectRegions(regions, count, cancelled, result);
    return result;
  }
} // namespace remix::forward::probes
