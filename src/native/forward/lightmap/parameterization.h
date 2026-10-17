#pragma once
#include "scene/bake_scene.h"
#include <stop_token>

namespace remix::forward::lightmap {
  struct Mapping {
    uint32_t width = 0, height = 0;
    std::vector<std::array<float, 2>> texcoords;
    // Planar charts use an orthonormal metre-space basis and a global grid.
    // Nonplanar charts retain the host's unwrapping.
    bool worldGrid = false;
    std::array<int64_t, 2> origin{};
    std::array<int64_t, 3> normalKey{};
    std::array<double, 3> u{}, v{};
  };
  std::vector<Mapping> parameterize(const scene::BakeScene &scene, float luxelMetres, uint32_t step,
                                    std::stop_token cancelled = {});
} // namespace remix::forward::lightmap
