#pragma once
#include "forward/bake/grid.h"
#include "forward/bake/receiver_projection.h"
#include "forward/lightmap/data.h"

namespace remix::forward::bake {
  struct ReceiverComparison {
    uint32_t chart = 0, draw = 0;
    int32_t cluster = -2; // unknown without a closed BSP; -1 is solid/outside
    Vec3 position{}, normal{}, surface{}, probe{};
    Vec4 coordinate{};
    bool covered = false;
    std::optional<float> containedBias; // closed-partition CPU oracle; absent means unknown/unavailable
    Vec3 containedProbe{};
    bool containedCovered = false;
  };
  // Bounded, deterministic triangle-centre samples of the same published
  // indirect atlas and volume used by shading. No material/direct-light terms.
  std::vector<ReceiverComparison> compareReceivers(const scene::BakeScene &source,
      const lightmap::Layout &layout, std::span<const uint16_t> pixels, const Grid &grid,
      uint32_t maximumSamples = 8192, std::stop_token cancelled = {});
  struct StyledDirectGroups;
  class ProbeVisibility;
  // Once per publication attempt; diagnostic allocation failure never fails baking.
  void logGroupedDirectDeclines(const scene::BakeScene &source, const ProbeVisibility &probes,
                                const StyledDirectGroups &groups) noexcept;
  void dumpGroupedDirectReport(const scene::BakeScene &source, const lightmap::Layout &layout,
      std::span<const uint16_t> pixels, const Grid &grid, std::span<const uint32_t> directionals,
      const StyledDirectGroups &groups, std::stop_token cancelled = {}) noexcept;
  // Optional worker-side diagnostic, also usable after an exact cache hit.
  // REMIX_DEBUG_BAKE_RECEIVERS names a fresh JSON file. Distant-light reports
  // also include complete charted receiver bounds in sun-space metres.
  // A closed host partition also supplies a CPU containment oracle, bounded
  // to 65536 probe records. It caps one coarse bias (fine queries inherit it),
  // reports unknown starts explicitly, and never changes published shading.
  // Failures never fail baking.
  void dumpReceiverReport(const scene::BakeScene &source, const lightmap::Layout &layout,
                          std::span<const uint16_t> pixels, const Grid &grid,
                          std::stop_token cancelled = {}) noexcept;
} // namespace remix::forward::bake
