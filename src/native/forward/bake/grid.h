#pragma once
#include "forward/bake/config.h"
#include "forward/bake/data.h"
#include <optional>
namespace remix::forward::lightmap {
  struct Layout;
}
namespace remix::forward::bake {
  struct GridHeader {
    Vec4 originSpacing{};
    UInt4 dimensions{};
    Vec4 settings{}; // surface bias, minimum distance variance, relocated-position flag, reserved
  };
  static_assert(sizeof(GridHeader) == 48);
  struct Grid {
    GridHeader header;
    std::vector<Point> points;
    std::vector<uint32_t> tasks;
    std::vector<Result> probes;
  };
  Grid place(const scene::BakeScene &scene, const Config &config);
  // Reuse untraced solid/outside slots near chart receivers. Each axis remains
  // within .45 cell widths of its original node; the allocation never grows.
  uint32_t relocate(Grid &grid, const scene::BakeScene &scene, const lightmap::Layout &layout,
                     std::stop_token cancelled = {});
  // Grid-only alpha slots 0..2 retain the displacement of the actual trace
  // origin. Surface/capture Result alpha channels keep their own contracts.
  void storeProbe(Grid &grid, uint32_t index, const Result &value);
  // CPU reference for the forward shader's visibility-aware reconstruction.
  std::optional<Vec3> sample(const Grid &grid, Vec3 position, Vec3 normal);
} // namespace remix::forward::bake
