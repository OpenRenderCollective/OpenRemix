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
  // Adaptive records use their largest referencing/trace spacing, preserving
  // coarse support for shared nodes without expanding every fine-only volume.
  // No visibility claim is made by distance, cone or BSP classification alone.
  // Select complete light volumes in increasing pair cost, then captured index.
  // Unselected lights remain unknown; no individual light's pairs are truncated.
  // Grouped direct can select controllable style groups atomically, ranked by
  // pair cost per member, so one missing member cannot strand a baked component.
  // Returns no plan when nonempty groups exist but none fits the budget.
  std::optional<ProbeVisibilityPlan> planProbeVisibility(const Grid &grid,
                                                         std::span<const scene::SphereLight> lights,
                                                         float cutoff, uint32_t maximumPairs,
                                                         std::stop_token cancelled = {},
                                                         const DirectPolicy &directLighting = {},
                                                         bool completeStyleGroups = false);
} // namespace remix::forward::bake
