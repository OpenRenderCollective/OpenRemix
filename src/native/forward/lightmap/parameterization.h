#pragma once
#include "scene/bake_scene.h"
#include <span>
#include <stop_token>

namespace remix::forward::lightmap {
  struct Mapping {
    uint32_t width = 0, height = 0;
    std::vector<std::array<float, 2>> texcoords;
    // Planar charts use an orthonormal metre-space basis and a global grid.
    // Curved charts measure spacing on the original surface geometry.
    bool worldGrid = false;
    bool curvedMetric = false, intrinsic = false;
    std::array<int64_t, 2> origin{};
    std::array<int64_t, 3> normalKey{};
    std::array<double, 3> u{}, v{};
  };
  struct ChartShape {
    Mapping frame;
    std::vector<std::array<double, 2>> coordinates; // metres, or authored luxels in legacy mode
  };
  // Expensive geometry analysis is independent of the atlas budget. Prepare it
  // once, then reuse it while fitting different global spacing multipliers.
  std::vector<ChartShape> prepareShapes(const scene::BakeScene &scene, bool worldMetric,
                                        std::stop_token cancelled);
  std::vector<Mapping> parameterize(std::span<const ChartShape> shapes, float luxelMetres, uint32_t step,
                                    std::stop_token cancelled);
} // namespace remix::forward::lightmap
