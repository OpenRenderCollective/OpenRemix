#pragma once
#include "forward/lighting.h"
#include "scene/bake_scene.h"

namespace remix::forward::shadows {
  // Static cascades belong to their exact frozen scene and fitted projection.
  // This cache is shared across frame slots, just like the shadow image: commands
  // are recorded and submitted in order on the renderer's graphics queue.
  class SunCache {
  public:
    std::array<bool, cascadeCount> update(const std::shared_ptr<const scene::BakeScene> &source,
                                         bool enabled, const std::array<Matrix, cascadeCount> &matrices) {
      const bool reset = !m_initialized || m_enabled != enabled || m_hasSource != bool(source) ||
                         m_source.lock() != source;
      std::array<bool, cascadeCount> dirty;
      for (uint32_t c = 0; c < cascadeCount; ++c)
        dirty[c] = reset || (enabled && m_matrices[c] != matrices[c]);
      m_source = source;
      m_hasSource = bool(source);
      m_enabled = enabled;
      m_matrices = matrices;
      m_initialized = true;
      return dirty;
    }

  private:
    std::weak_ptr<const scene::BakeScene> m_source;
    std::array<Matrix, cascadeCount> m_matrices{};
    bool m_enabled = false, m_initialized = false, m_hasSource = false;
  };
} // namespace remix::forward::shadows
