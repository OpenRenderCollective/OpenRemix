#pragma once
#include "forward/lighting.h"

namespace remix::forward::probes {
  // Reflection placement needs geometry, never diffuse SH coefficients.
  // Keep original indices and task order for deterministic distance tie breaks.
  struct PlacementSeeds {
    struct Point { Vec3 position{}; bool usable = false; };
    Bounds bounds;
    std::vector<Point> points;
    std::vector<uint32_t> tasks;
  };
}
