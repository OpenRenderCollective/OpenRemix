#pragma once
#include "forward/bake/grid.h"

namespace remix::forward::bake {
  struct Refinement {
    uint32_t factor = 0, recordBudget = 0;
    bool relocate = true;
  };
  // Reserve part of the record budget for a second, measured refinement pass.
  // Small/unpartitioned fields keep the existing complete sparse/dense policy.
  Grid placeRefinementBase(const scene::BakeScene &scene, const Config &config,
                            const lightmap::Layout &layout, Refinement &refinement,
                            std::stop_token cancelled = {});
  // Called only after every coarse result has arrived. Appends trace tasks;
  // captured surfaces, scene membership and existing results remain unchanged.
  uint32_t refineGrid(Grid &grid, const scene::BakeScene &scene, const lightmap::Layout &layout,
                       const Refinement &refinement, std::span<const Result> surfaceReference = {},
                       std::stop_token cancelled = {});
} // namespace remix::forward::bake
