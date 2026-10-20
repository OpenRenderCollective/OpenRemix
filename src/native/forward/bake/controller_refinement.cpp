#include "forward/bake/job.h"
#include <cstdio>

namespace remix::forward::bake {
  void Controller::Job::refineProbes(const Config &config) {
    const auto began = std::chrono::steady_clock::now();
    const auto coarseRecords = grid.probes.size();
    const auto patches = refineGrid(grid, *source, surfaceLayout, refinement, surfaces, cancellation.get_token());
    if (!patches) {
      // If the spatial partition admits no useful fine nodes, restore the full
      // ordinary placement budget instead of publishing a half-density field.
      auto placement = config;
      placement.maxProbes = refinement.recordBudget;
      grid = placeSparse(*source, placement, surfaceLayout, cancellation.get_token());
      next = received = 0;
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - began).count();
    std::array<uint32_t, 4> levels{};
    uint32_t finest = 1;
    if (patches) {
      const auto nodes = gridNodes(grid.header);
      for (uint32_t cell = 0; cell < nodes; ++cell) {
        const auto reference = grid.lookup[nodes + 4 + cell];
        if (!reference) continue;
        const auto factor = patchFactor(reference, grid.lookup[nodes + 1]);
        ++levels[std::countr_zero(factor)];
        finest = std::max(finest, factor);
      }
    }
    std::fprintf(stderr, "Forward adaptive irradiance: %zu coarse records, %u patches, %zu final records, "
                         "%.3f -> %.3f m spacing, %.3f ms planning; levels 2x:%u 4x:%u 8x:%u%s\n",
                 coarseRecords, patches, grid.probes.size(), grid.header.originSpacing[3] * source->sceneScale,
                 grid.header.originSpacing[3] * source->sceneScale / finest,
                 elapsed, levels[1], levels[2], levels[3], patches ? "" : "; full-density fallback");
    finishStage.store(FinishStage::None, std::memory_order_relaxed);
  }
} // namespace remix::forward::bake
