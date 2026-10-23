#pragma once
#include "forward/bake/grid.h"
#include "forward/lightmap/data.h"

namespace remix::forward::bake {
  struct ReceiverComparison {
    uint32_t chart = 0, draw = 0;
    int32_t cluster = -2; // unknown without a closed BSP; -1 is solid/outside
    Vec3 position{}, normal{}, surface{}, probe{};
    Vec4 coordinate{};
    bool covered = false;
  };
  // Bounded, deterministic triangle-centre samples of the same published
  // indirect atlas and volume used by shading. No material/direct-light terms.
  std::vector<ReceiverComparison> compareReceivers(const scene::BakeScene &source,
      const lightmap::Layout &layout, std::span<const uint16_t> pixels, const Grid &grid,
      uint32_t maximumSamples = 8192, std::stop_token cancelled = {});
  struct ReceiverProjection {
    Bounds bounds; // metres in the supplied orthonormal basis
    uint64_t triangles = 0;
  };
  // Bounds every vertex of charted receiver triangles, including cached mappings.
  // Excludes uncharted triangles; this is receiver coverage, not caster coverage.
  ReceiverProjection projectReceivers(const scene::BakeScene &source, const lightmap::Layout &layout,
      const std::array<Vec3, 3> &basis, std::stop_token cancelled = {});
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
  // Failures never fail baking.
  void dumpReceiverReport(const scene::BakeScene &source, const lightmap::Layout &layout,
                          std::span<const uint16_t> pixels, const Grid &grid,
                          std::stop_token cancelled = {}) noexcept;
} // namespace remix::forward::bake
