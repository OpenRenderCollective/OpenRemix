#include "forward/probes/placement_seeds.h"
#include "scene/bake_scene.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::probes {
  PlacementSeeds geometricSeedCandidates(const scene::BakeScene &source, uint32_t budget,
                                          std::stop_token cancelled) {
    if (!budget || budget > 4096 || !std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid reflection geometry seed budget or scale");
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Reflection seed planning superseded");
    };
    check();
    PlacementSeeds result;
    if (source.hasBounds) {
      for (uint32_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(source.minimum[axis]) || !std::isfinite(source.maximum[axis]) ||
            source.minimum[axis] > source.maximum[axis])
          throw std::invalid_argument("Invalid reflection placement bounds");
      result.bounds.include(source.minimum);
      result.bounds.include(source.maximum);
    } else {
      for (const auto &draw : source.draws) {
        check();
        if (!draw.surface) continue;
        for (size_t i = 0; i < draw.surface->vertices.size(); ++i) {
          if ((i & 4095u) == 0) check();
          const auto &v = draw.surface->vertices[i].position;
          result.bounds.include(transformPoint(draw.model, {v[0], v[1], v[2]}));
        }
      }
    }
    if (!result.bounds.valid) throw std::invalid_argument("Reflection placement needs bounds or static geometry");
    Vec3 extent;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      auto &lo = result.bounds.minimum[axis], &hi = result.bounds.maximum[axis];
      if (!std::isfinite(lo) || !std::isfinite(hi) || hi < lo)
        throw std::invalid_argument("Invalid reflection placement bounds");
      if (hi == lo) { lo -= .5f / source.sceneScale; hi += .5f / source.sceneScale; }
      extent[axis] = hi - lo;
      if (!std::isfinite(extent[axis]) || extent[axis] <= 0)
        throw std::invalid_argument("Reflection placement extent exceeds precision");
    }
    // Spend cells along the longest remaining dimension. Corridor-shaped scenes
    // retain longitudinal coverage instead of spending a cubic grid in walls.
    std::array<uint32_t, 3> dimensions{1, 1, 1};
    uint32_t cells = 1;
    while (cells < budget) {
      uint32_t selected = 3;
      double longest = 0;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const uint32_t next = cells / dimensions[axis] * (dimensions[axis] + 1);
        const double length = double(extent[axis]) / dimensions[axis];
        if (next <= budget && length > longest) { selected = axis; longest = length; }
      }
      if (selected == 3) break;
      cells = cells / dimensions[selected] * (dimensions[selected] + 1);
      ++dimensions[selected];
    }
    result.points.reserve(cells);
    for (uint32_t z = 0; z < dimensions[2]; ++z)
      for (uint32_t y = 0; y < dimensions[1]; ++y)
        for (uint32_t x = 0; x < dimensions[0]; ++x) {
          check();
          const std::array<uint32_t, 3> cell{x,y,z};
          Vec3 position;
          for (uint32_t axis = 0; axis < 3; ++axis)
            position[axis] = float(double(result.bounds.minimum[axis]) +
                (double(cell[axis]) + .5) * extent[axis] / dimensions[axis]);
          result.points.push_back({position, uint32_t(result.points.size())});
        }
    return result;
  }
}
