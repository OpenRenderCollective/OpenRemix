#include "forward/probes/placement.h"
#include "forward/probes/regions.h"
#include "forward/probes/selection.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>

namespace remix::forward::probes {
  namespace {
    Bounds placementBounds(const scene::BakeScene &scene, const bake::Grid &grid) {
      Bounds bounds;
      const auto &h = grid.header;
      for (uint32_t c = 0; c < 3; ++c) {
        bounds.minimum[c] = scene.hasBounds ? scene.minimum[c] : h.originSpacing[c] + h.originSpacing[3];
        bounds.maximum[c] = scene.hasBounds
                                ? scene.maximum[c]
                                : h.originSpacing[c] + float(h.dimensions[c] - 2) * h.originSpacing[3];
        if (bounds.maximum[c] <= bounds.minimum[c]) {
          bounds.minimum[c] -= h.originSpacing[3] * .5f;
          bounds.maximum[c] += h.originSpacing[3] * .5f;
        }
      }
      bounds.valid = true;
      return bounds;
    }
  } // namespace
  void configureInfluence(const scene::BakeScene &scene,
                          std::span<Capture> captures, float detailTolerance,
                          std::stop_token cancelled) {
    if (!std::isfinite(scene.sceneScale) || scene.sceneScale <= 0 ||
        !std::isfinite(detailTolerance) || detailTolerance < 0)
      throw std::invalid_argument(
          "Invalid reflection influence scale or tolerance");
    const float distance = .5f / scene.sceneScale;
    for (auto &capture : captures) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection influence preparation superseded");
      capture.blendDistance = distance;
      if (scene.partition && scene.partition->closed &&
          capture.worldToLocal == worldSpace)
        capture.influence = receiverRegion(
            *scene.partition, capture.bounds, .002f / scene.sceneScale,
            cancelled, {detailTolerance / scene.sceneScale}, distance);
    }
  }
  std::vector<Vec3> candidateSites(const scene::BakeScene &scene, const bake::Grid &grid, uint32_t count,
                                   std::stop_token cancelled) {
    if (!count || !scene.partition || !scene.partition->closed)
      return {};
    const auto bounds = placementBounds(scene, grid);
    const float clearance = .05f / scene.sceneScale, range = 64.f / scene.sceneScale;
    std::vector<RegionSite> ranked;
    for (const auto &hint : scene.reflectionHints) {
      const auto site = regionSite(*scene.partition, hint, clearance, range, cancelled);
      if (site)
        ranked.push_back(*site);
    }
    const auto automatic =
        regionSites(*scene.partition, bounds, clearance, range, cancelled);
    ranked.insert(ranked.end(), automatic.begin(), automatic.end());
    // Sheltered rooms need local parallax more than open areas. Preserve authored
    // preference within each enclosure class, rather than consuming every slot
    // with the first outdoor env_cubemap entities in BSP order.
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
      return a.enclosedAxes > b.enclosedAxes;
    });
    std::vector<Vec3> result;
    result.reserve(ranked.size());
    for (const auto &site : ranked)
      result.push_back(site.position);
    return result;
  }
  std::vector<FittedRegion> fitCandidates(const scene::BakeScene &scene, const bake::Grid &grid,
                                          uint32_t count, std::stop_token cancelled, float detailTolerance,
                                          const std::vector<Vec3> *validatedSites) {
    if (count > maxProbes || !std::isfinite(detailTolerance) || detailTolerance < 0)
      throw std::invalid_argument("Too many reflection probes");
    if (!count)
      return {};
    const auto bounds = placementBounds(scene, grid);
    std::vector<Bounds> boxes{bounds};
    while (boxes.size() < count) {
      size_t selected = 0;
      uint32_t axis = 0;
      float longest = 0;
      for (size_t i = 0; i < boxes.size(); ++i)
        for (uint32_t c = 0; c < 3; ++c) {
          const float length = boxes[i].maximum[c] - boxes[i].minimum[c];
          if (length > longest) {
            selected = i;
            axis = c;
            longest = length;
          }
        }
      auto second = boxes[selected];
      const float middle = std::midpoint(second.minimum[axis], second.maximum[axis]);
      boxes[selected].maximum[axis] = middle;
      second.minimum[axis] = middle;
      boxes.push_back(second);
    }
    std::vector<FittedRegion> regions, alternatives;
    const bool fitted = scene.partition && scene.partition->closed;
    auto covered = [&](Vec3 position) {
      return std::any_of(regions.begin(), regions.end(), [&](const auto &capture) {
        for (uint32_t c = 0; c < 3; ++c)
          if (position[c] < capture.bounds.minimum[c] || position[c] > capture.bounds.maximum[c])
            return false;
        return true;
      });
    };
    // Production passes GPU-validated sites. Direct CPU callers can exercise
    // geometry fitting independently, with the same bounded candidate policy.
    const auto generated =
        validatedSites ? std::vector<Vec3>{} : candidateSites(scene, grid, count, cancelled);
    const auto &sites = validatedSites ? *validatedSites : generated;
    // Keep expensive BSP fitting bounded even when a map uses more captures.
    const uint32_t budget = std::clamp(count * 8, 32u, 128u);
    // Larger resident sets do not expand the total geometry-audit cap. Up to
    // 128 fitted origins leave the same bounded room for grid fallbacks.
    const uint32_t auditLimit = budget + std::min(count, 32u);
    if (fitted) {
      std::vector<RegionSite> assessed;
      std::set<Vec3> seen;
      for (const auto &position : sites) {
        if (!seen.insert(position).second)
          continue;
        if (const auto site = regionSite(*scene.partition, position, .05f / scene.sceneScale,
                                          64.f / scene.sceneScale, cancelled))
          assessed.push_back(*site);
      }
      for (auto index : spreadSites(assessed, budget, 4.f / scene.sceneScale, cancelled)) {
        const auto &site = assessed[index];
        const auto fits = fitRegionCandidates(*scene.partition, bounds, site.position, .001f / scene.sceneScale,
                                               cancelled, {detailTolerance / scene.sceneScale});
        if (!fits.empty()) regions.push_back({fits.front(), site});
        for (size_t fit = 1; fit < fits.size(); ++fit)
          alternatives.push_back({fits[fit], site});
      }
    }
    for (auto box : boxes) {
      if (regions.size() == auditLimit)
        break;
      Vec3 center;
      for (uint32_t c = 0; c < 3; ++c)
        center[c] = std::midpoint(box.minimum[c], box.maximum[c]);
      std::vector<std::pair<float, uint32_t>> candidates;
      for (uint32_t index : grid.tasks) {
        if (grid.probes[index].statistics[3] < .5f)
          continue;
        const auto &p = grid.points[index].position;
        bool inside = true;
        float distance = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          inside &= p[c] >= box.minimum[c] && p[c] <= box.maximum[c];
          const float delta = p[c] - center[c];
          distance += delta * delta;
        }
        if (inside)
          candidates.emplace_back(distance, index);
      }
      std::sort(candidates.begin(), candidates.end());
      uint32_t selected = ~0u;
      for (size_t candidate = 0; candidate < std::min(size_t(16), candidates.size()); ++candidate) {
        if (cancelled.stop_requested())
          throw std::runtime_error("Reflection placement superseded");
        const auto index = candidates[candidate].second;
        const auto &p = grid.points[index].position;
        if (fitted) {
          if (covered({p[0], p[1], p[2]}))
            continue;
          const auto region =
              fitRegion(*scene.partition, bounds, {p[0], p[1], p[2]}, .001f / scene.sceneScale, cancelled,
                        {detailTolerance / scene.sceneScale});
          if (!region)
            continue;
          box = *region;
        }
        selected = index;
        break;
      }
      if (selected == ~0u)
        continue;
      // Without occupancy data retain the coarse, overlapping fallback. Fitted
      // regions retain their wall constraints, including with detail tolerance.
      for (uint32_t c = 0; !fitted && c < 3; ++c) {
        const float padding = (box.maximum[c] - box.minimum[c]) * .1f;
        box.minimum[c] = std::max(bounds.minimum[c], box.minimum[c] - padding);
        box.maximum[c] = std::min(bounds.maximum[c], box.maximum[c] + padding);
      }
      const auto &p = grid.points[selected].position;
      regions.push_back({box, {{p[0], p[1], p[2]}, 0, 0}});
    }
    // Preserve origin coverage first. Remaining audit slots can compare shape
    // alternatives without increasing the established budget (at most 160).
    for (const auto &alternative : alternatives) {
      if (regions.size() == auditLimit) break;
      regions.push_back(alternative);
    }
    return regions;
  }
  std::vector<Capture> captureCandidates(const scene::BakeScene &scene, std::span<const FittedRegion> regions,
                                         uint32_t count, uint32_t resolution, std::stop_token cancelled,
                                         float detailTolerance) {
    if (count > maxProbes || !std::isfinite(detailTolerance) || detailTolerance < 0)
      throw std::invalid_argument("Invalid reflection capture budget");
    std::vector<Capture> captures;
    for (auto index : selectRegions(regions, count, cancelled)) {
      const auto &region = regions[index];
      Capture capture{region.bounds, region.site.position, resolution, {}, region.bounds};
      capture.radiance.resize(rays(capture).size());
      captures.push_back(std::move(capture));
    }
    configureInfluence(scene, captures, detailTolerance, cancelled);
    return captures;
  }
  std::vector<Capture> place(const scene::BakeScene &scene, const bake::Grid &grid, uint32_t count,
                             uint32_t resolution, std::stop_token cancelled, float detailTolerance,
                             const std::vector<Vec3> *validatedSites) {
    const auto regions = fitCandidates(scene, grid, count, cancelled, detailTolerance, validatedSites);
    return captureCandidates(scene, regions, count, resolution, cancelled, detailTolerance);
  }
} // namespace remix::forward::probes
