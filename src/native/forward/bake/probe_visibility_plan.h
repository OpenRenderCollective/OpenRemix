#pragma once
#include "forward/bake/grid.h"
#include "forward/bake/policy.h"
#include <stop_token>

namespace remix::forward::bake {
  struct ProbeVisibilityPlan {
    std::vector<VisibilityTask> tasks;
    uint64_t candidates = 0; // broad-phase tests, not the full probe/light product
    uint32_t eligibleLights = 0, selectedLights = 0, deferredLights = 0;
  };
  // Conservative candidates for wall-aware trilinear reconstruction. Uses
  // actual relocated positions; halo covers all corners of a receiver cell.
  // No visibility claim is made by distance, cone or BSP classification alone.
  // Select complete light volumes in increasing pair cost, then captured index.
  // Unselected lights remain unknown; no individual light's pairs are truncated.
  // Returns no plan when nonempty groups exist but none fits the budget.
  std::optional<ProbeVisibilityPlan> planProbeVisibility(const Grid &grid,
                                                         std::span<const scene::SphereLight> lights,
                                                         float cutoff, uint32_t maximumPairs,
                                                         std::stop_token cancelled = {},
                                                         const DirectPolicy &directLighting = {});
} // namespace remix::forward::bake
