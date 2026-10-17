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
                                  std::stop_token cancelled) {
    check(cancelled);
    if (!std::isfinite(clearanceCap) || clearanceCap <= 0)
      throw std::invalid_argument("Invalid reflection site clearance cap");
    std::vector<size_t> result;
    std::vector<bool> used(sites.size());
    std::vector<double> distance(sites.size(), std::numeric_limits<double>::infinity());
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
      used[best] = true;
      result.push_back(best);
      for (size_t i = 0; i < sites.size(); ++i) {
        double squared = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          const double delta = double(sites[i].position[c]) - sites[best].position[c];
          squared += delta * delta;
        }
        distance[i] = std::min(distance[i], squared);
      }
    }
    // Fit in original priority order so equal-coverage ties remain predictable.
    std::sort(result.begin(), result.end());
    return result;
  }
  std::vector<size_t> selectRegions(std::span<const FittedRegion> regions, uint32_t count,
                                    std::stop_token cancelled) {
    check(cancelled);
    std::vector<std::vector<Bounds>> uncovered;
    for (const auto &region : regions)
      uncovered.push_back({region.bounds});
    std::vector<size_t> result;
    while (result.size() < std::min(size_t(count), regions.size())) {
      check(cancelled);
      size_t best = regions.size();
      double bestScore = 0;
      for (size_t i = 0; i < regions.size(); ++i) {
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
} // namespace remix::forward::probes
