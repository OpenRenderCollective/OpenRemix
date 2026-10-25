#pragma once
#include "forward/lightmap/data.h"
#include <map>
#include <optional>

namespace remix::forward::lightmap {
  // A fixed receiver's oriented triangle directory. Live index-buffer subsets
  // can change without changing the authoritative bake or its chart ownership.
  class ReceiverTriangles {
  public:
    static constexpr uint32_t maximumIndices = 3u * 65536;
    explicit ReceiverTriangles(const Mesh &source, std::stop_token cancelled = {});
    std::optional<Mesh> map(const scene::Draw &captured, const scene::Draw &live, float sceneScale) const;

  private:
    using Position = std::array<float, 3>;
    using Key = std::array<Position, 3>;
    struct Triangle {
      std::array<Vertex, 3> corners;
      bool ambiguous = false;
    };
    std::map<Key, Triangle> m_triangles;
    uint32_t m_draw = 0;
  };
}
