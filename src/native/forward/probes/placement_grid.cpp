#include "forward/probes/placement.h"
#include "forward/bake/probe_origin.h"
#include <cmath>
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
  }
  PlacementSeeds placementSeeds(const scene::BakeScene &scene, const bake::Grid &grid,
                                 std::stop_token cancelled) {
    PlacementSeeds result;
    result.bounds = placementBounds(scene, grid);
    result.points.reserve(grid.tasks.size());
    for (uint32_t index : grid.tasks) {
      if ((index & 4095u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Reflection placement superseded");
      if (index >= grid.probes.size())
        throw std::invalid_argument("Reflection seed index is outside captured geometry");
      if (grid.probes[index].statistics[3] < .5f) continue;
      Vec3 position;
      if (index < grid.points.size()) {
        const auto &point = grid.points[index].position;
        position = {point[0], point[1], point[2]};
      } else {
        // Complete cache hits omit transient Point arrays. Recover the admitted
        // origin directly, retaining only the compact reflection metadata.
        if (!grid.points.empty())
          throw std::invalid_argument("Usable reflection seed has no captured position");
        position = bake::probeOrigin(grid, index);
      }
      result.points.push_back({position, index});
    }
    return result;
  }
  std::vector<Vec3> candidateSites(const scene::BakeScene &scene, const bake::Grid &grid,
                                   uint32_t count, std::stop_token cancelled) {
    if (!count || !scene.partition || !scene.partition->closed) return {};
    PlacementSeeds domain;
    domain.bounds = placementBounds(scene, grid);
    return candidateSeedSites(scene, domain, count, cancelled);
  }
  std::vector<FittedRegion> fitCandidates(const scene::BakeScene &scene, const bake::Grid &grid,
      uint32_t count, std::stop_token cancelled, float detailTolerance, const std::vector<Vec3> *validatedSites) {
    if (!count) return fitSeedCandidates(scene, {}, count, cancelled, detailTolerance, validatedSites);
    return fitSeedCandidates(scene, placementSeeds(scene, grid, cancelled), count, cancelled,
                              detailTolerance, validatedSites);
  }
  std::vector<Capture> place(const scene::BakeScene &scene, const bake::Grid &grid, uint32_t count,
                             uint32_t resolution, std::stop_token cancelled, float detailTolerance,
                             const std::vector<Vec3> *validatedSites) {
    const auto regions = fitCandidates(scene, grid, count, cancelled, detailTolerance, validatedSites);
    return captureCandidates(scene, regions, count, resolution, cancelled, detailTolerance);
  }
}
