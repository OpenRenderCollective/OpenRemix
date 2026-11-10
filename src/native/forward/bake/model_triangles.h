#pragma once
#include "scene/bake_scene.h"
#include <functional>

namespace remix::forward::bake {
  struct ModelTriangle {
    uint32_t draw = 0, firstIndex = 0;
    std::array<std::array<float, 3>, 3> positions{}; // transformed host coordinates
    double areaSquareMetres = 0;
  };
  // Streams eligible uncharted static triangles in source draw/index order,
  // independently of chart allocation. A callback must copy retained data.
  void forEachModelTriangle(const scene::BakeScene &source,
      const std::function<void(const ModelTriangle &)> &visit, std::stop_token cancelled = {});
}
