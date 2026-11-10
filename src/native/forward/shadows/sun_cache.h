#pragma once
#include "forward/lighting.h"
#include "scene/bake_scene.h"

namespace remix::forward::shadows {
  // Static cascades belong to their exact frozen scene and fitted projection.
  // This cache is shared across frame slots, just like the shadow image: commands
  // are recorded and submitted in order on the renderer's graphics queue.
  class SunCache {
  public:
    // Reused maps must be sampled and rendered with the same matrix. Tiny fit
    // noise can reuse the prior projection inside its existing texel padding.
    std::array<bool, cascadeCount> update(const std::shared_ptr<const scene::BakeScene> &source,
                                         bool enabled, std::array<Matrix, cascadeCount> &matrices,
                                         uint32_t resolution = 1024);

  private:
    std::weak_ptr<const scene::BakeScene> m_source;
    std::array<Matrix, cascadeCount> m_matrices{};
    uint32_t m_resolution = 0;
    bool m_enabled = false, m_initialized = false, m_hasSource = false;
  };
} // namespace remix::forward::shadows
