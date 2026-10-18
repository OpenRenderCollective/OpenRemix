#pragma once
#include "forward/bake/grid.h"
#include <stop_token>

namespace remix::forward::bake {
  struct ProbeVisibilityPlan {
    std::vector<VisibilityTask> tasks;
    uint64_t candidates = 0; // broad-phase tests, not the full probe/light product
  };
  // Conservative candidates for wall-aware trilinear reconstruction. Uses
  // actual relocated positions; halo covers all corners of a receiver cell.
  // No visibility claim is made by distance, cone or BSP classification alone.
  // A budget miss returns no plan, so callers must retain complete fallback.
  std::optional<ProbeVisibilityPlan> planProbeVisibility(const Grid &grid,
                                                         std::span<const scene::SphereLight> lights,
                                                         float cutoff, uint32_t maximumPairs,
                                                         std::stop_token cancelled = {});
} // namespace remix::forward::bake
