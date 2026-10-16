#include "forward/probes/data.h"
#include "forward/probes/regions.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
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
  std::vector<Vec3> candidateSites(const scene::BakeScene &scene, const bake::Grid &grid, uint32_t count,
                                   std::stop_token cancelled) {
    if (!count || !scene.partition || !scene.partition->closed)
      return {};
    const auto bounds = placementBounds(scene, grid);
    std::vector<Vec3> result = scene.reflectionHints;
    const auto automatic =
        regionSites(*scene.partition, bounds, .05f / scene.sceneScale, 64.f / scene.sceneScale, cancelled);
    for (const auto &site : automatic)
      result.push_back(site.position);
    return result;
  }
  std::vector<Capture> place(const scene::BakeScene &scene, const bake::Grid &grid, uint32_t count,
                             uint32_t resolution, std::stop_token cancelled, float detailTolerance,
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
    std::vector<Capture> captures;
    const bool fitted = scene.partition && scene.partition->closed;
    auto duplicate = [&](const Bounds &region) {
      for (const auto &capture : captures) {
        double overlap = 1, a = 1, b = 1;
        for (uint32_t c = 0; c < 3; ++c) {
          overlap *= std::max(0., double(std::min(region.maximum[c], capture.bounds.maximum[c])) -
                                      std::max(region.minimum[c], capture.bounds.minimum[c]));
          a *= double(region.maximum[c]) - region.minimum[c];
          b *= double(capture.bounds.maximum[c]) - capture.bounds.minimum[c];
        }
        if (overlap > .95 * std::min(a, b))
          return true;
      }
      return false;
    };
    auto append = [&](Bounds box, Vec3 position) {
      Capture capture{box, position, resolution, {}, box};
      capture.radiance.resize(rays(capture).size());
      captures.push_back(std::move(capture));
    };
    auto covered = [&](Vec3 position) {
      return std::any_of(captures.begin(), captures.end(), [&](const auto &capture) {
        for (uint32_t c = 0; c < 3; ++c)
          if (position[c] < capture.bounds.minimum[c] || position[c] > capture.bounds.maximum[c])
            return false;
        return true;
      });
    };
    auto fit = [&](Vec3 position) {
      const auto region = fitRegion(*scene.partition, bounds, position, .001f / scene.sceneScale, cancelled,
                                    {detailTolerance / scene.sceneScale});
      if (region && !duplicate(*region))
        append(*region, position);
    };
    // Production passes GPU-validated sites. Direct CPU callers can exercise
    // geometry fitting independently, with the same bounded candidate policy.
    const auto generated =
        validatedSites ? std::vector<Vec3>{} : candidateSites(scene, grid, count, cancelled);
    const auto &sites = validatedSites ? *validatedSites : generated;
    uint32_t attempts = 0;
    const uint32_t budget = std::max(32u, count * 8);
    for (const auto &position : sites) {
      if (!fitted || captures.size() == count || attempts == budget)
        break;
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection placement superseded");
      if (covered(position))
        continue;
      ++attempts;
      fit(position);
    }
    for (auto box : boxes) {
      if (captures.size() == count)
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
          if (duplicate(*region))
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
      append(box, {p[0], p[1], p[2]});
    }
    return captures;
  }
} // namespace remix::forward::probes
