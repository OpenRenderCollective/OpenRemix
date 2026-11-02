#pragma once
#include "forward/lighting.h"

namespace remix::forward::probes {
  // Reflection placement needs geometry, never diffuse SH coefficients.
  // Retain usable points in task order, with original indices for distance ties.
  struct PlacementSeeds {
    struct Point { Vec3 position{}; uint32_t originalIndex = 0; };
    Bounds bounds;
    std::vector<Point> points;
  };
}
