#pragma once
#include "forward/bake/grid.h"

namespace remix::forward::bake {
  // One planning pass over immutable receivers. Neighbouring patches reuse the
  // same clearance query; changing clearance invalidates only this CPU scratch.
  class RelocationCache {
  public:
    RelocationCache(const scene::BakeScene &scene, const lightmap::Layout &layout);
    void clearance(float value);
    std::optional<Vec4> position(uint32_t receiver);
    uint64_t queries() const { return m_queries; }
    uint64_t hits() const { return m_hits; }

  private:
    const scene::BakeScene &m_scene;
    const lightmap::Layout &m_layout;
    float m_clearance = -1;
    std::vector<Vec4> m_positions;
    std::vector<uint8_t> m_states; // 0 unqueried, 1 admissible, 2 inadmissible
    uint64_t m_queries = 0, m_hits = 0;
  };
}
