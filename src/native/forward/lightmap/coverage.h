#pragma once
#include "forward/lightmap/layout.h"
#include <map>
#include <span>
#include <tuple>

namespace remix::forward::lightmap {
  // A coarse spatial index answers exact segment/triangle coverage queries.
  // A connected patch can contain holes: connectivity alone does not permit
  // borrowing a sample across an opening elsewhere in that patch.
  class Coverage {
  public:
    using Point = std::array<double, 2>;
    Coverage(const scene::BakeScene &scene, const Layout &layout, std::span<const uint32_t> patches,
             std::stop_token cancelled);
    bool covers(uint32_t patch, Point start, Point end);

  private:
    struct Triangle {
      std::array<Point, 3> points;
    };
    static constexpr double cellSize = 8; // grid luxels, independent of world scale
    using Key = std::tuple<uint32_t, int64_t, int64_t>;
    double m_tolerance;
    std::vector<Triangle> m_triangles;
    std::map<Key, std::vector<uint32_t>> m_cells;
    std::vector<uint32_t> m_candidates;
    std::vector<std::array<double, 2>> m_intervals;
  };
} // namespace remix::forward::lightmap
