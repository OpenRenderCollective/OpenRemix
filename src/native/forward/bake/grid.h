#pragma once
#include "forward/bake/config.h"
#include "forward/bake/data.h"
#include <optional>
namespace remix::forward::bake {
  struct GridHeader {
    Vec4 originSpacing{};
    UInt4 dimensions{};
    Vec4 settings{}; // surface bias, minimum distance variance, reserved
  };
  static_assert(sizeof(GridHeader) == 48);
  struct Grid {
    GridHeader header;
    std::vector<Point> points;
    std::vector<uint32_t> tasks;
    std::vector<Result> probes;
  };
  Grid place(const scene::BakeScene &scene, const Config &config);
  // CPU reference for the forward shader's visibility-aware reconstruction.
  std::optional<Vec3> sample(const Grid &grid, Vec3 position, Vec3 normal);
} // namespace remix::forward::bake
