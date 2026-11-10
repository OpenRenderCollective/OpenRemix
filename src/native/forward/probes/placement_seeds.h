#pragma once
#include "forward/lighting.h"
#include <stop_token>

namespace remix::scene { struct BakeScene; }

namespace remix::forward::probes {
  // Reflection placement needs geometry, never diffuse SH coefficients.
  // Retain usable points in task order, with original indices for distance ties.
  struct PlacementSeeds {
    struct Point { Vec3 position{}; uint32_t originalIndex = 0; };
    Bounds bounds;
    std::vector<Point> points;
  };
  // Bounded geometric candidates; callers must validate occupancy before fitting.
  PlacementSeeds geometricSeedCandidates(const scene::BakeScene &source, uint32_t budget,
                                          std::stop_token cancelled = {});
}
