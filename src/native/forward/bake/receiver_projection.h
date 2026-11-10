#pragma once
#include "forward/bake/grid.h"
#include "forward/bake/sun_depth_layout.h"
namespace remix::forward::bake {
  struct ReceiverProjection {
    Bounds bounds; // metres in the supplied orthonormal basis
    uint64_t triangles = 0;
    std::vector<ReceiverTileCoverage> tiles;
    std::vector<std::optional<SunDepthLayout>> depthLayouts;
  };
  // Bounds every vertex of charted receiver triangles, including cached mappings.
  // Excludes uncharted triangles; this is receiver coverage, not caster coverage.
  ReceiverProjection projectReceivers(const scene::BakeScene &source, const lightmap::Layout &layout,
      const std::array<Vec3, 3> &basis, std::stop_token cancelled = {},
      std::span<const ReceiverTileOptions> tiles = {});
}
