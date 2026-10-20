#pragma once
#include "forward/bake/adaptive_grid.h"

namespace remix::forward::bake {
  // Builds coverage at each subdivision level before attempting the next.
  // Fine records are still untraced here, so upgrades can compact discarded
  // records while preserving the completed coarse prefix exactly.
  uint32_t buildRefinement(Grid &grid, const scene::BakeScene &scene, const lightmap::Layout &layout,
                           const Refinement &policy, std::span<const std::vector<uint32_t>> receivers,
                           std::span<const uint32_t> priority, std::stop_token cancelled);
}
