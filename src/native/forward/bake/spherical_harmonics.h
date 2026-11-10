#pragma once
#include "forward/lighting.h"

namespace remix::forward::bake {
  // Unit normal required, as in the shader. Real L2 order matches the stored
  // cosine-convolved coefficients; normalization belongs to the caller.
  inline std::array<float, 9> irradianceBasis(Vec3 n) {
    return {.2820947918f, .4886025119f * n[1], .4886025119f * n[2], .4886025119f * n[0],
            1.092548431f * n[0] * n[1], 1.092548431f * n[1] * n[2],
            .3153915653f * (3 * n[2] * n[2] - 1), 1.092548431f * n[0] * n[2],
            .5462742153f * (n[0] * n[0] - n[1] * n[1])};
  }
} // namespace remix::forward::bake
