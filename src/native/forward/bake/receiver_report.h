#pragma once
#include "forward/bake/grid.h"
#include "forward/lightmap/data.h"

namespace remix::forward::bake {
  struct ReceiverComparison {
    uint32_t chart = 0, draw = 0;
    int32_t cluster = -2; // unknown without a closed BSP; -1 is solid/outside
    Vec3 position{}, normal{}, surface{}, probe{};
    bool covered = false;
  };
  // Bounded, deterministic triangle-centre samples of the same published
  // indirect atlas and volume used by shading. No material/direct-light terms.
  std::vector<ReceiverComparison> compareReceivers(const scene::BakeScene &source,
      const lightmap::Layout &layout, std::span<const uint16_t> pixels, const Grid &grid,
      uint32_t maximumSamples = 8192, std::stop_token cancelled = {});
  // Optional worker-side diagnostic, also usable after an exact cache hit.
  // REMIX_DEBUG_BAKE_RECEIVERS names a fresh JSON file. Failures never fail baking.
  void dumpReceiverReport(const scene::BakeScene &source, const lightmap::Layout &layout,
                          std::span<const uint16_t> pixels, const Grid &grid,
                          std::stop_token cancelled = {}) noexcept;
} // namespace remix::forward::bake
