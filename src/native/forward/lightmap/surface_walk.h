#pragma once
#include "forward/lightmap/layout.h"
#include <optional>

namespace remix::forward::lightmap::seam {
  using Point = std::array<double, 3>;
  using Uv = std::array<double, 2>;
  using Coordinates = std::array<Uv, 3>;
  struct Triangle {
    uint32_t chart = 0;
    std::array<Point, 3> positions, normals; // world metres, unit directions
    Coordinates uv;
    Point facing;
    std::array<std::vector<uint32_t>, 3> neighbours; // triangle * 3 + edge
  };
  // Chart-local topology is exact. Cross-chart links require overlapping edges
  // on the same world line, compatible facing and opposite triangle interiors.
  // Multiple links on one edge represent nonconforming T-junctions.
  class SurfaceWalk {
  public:
    SurfaceWalk(const scene::BakeScene &scene, const Layout &layout, std::stop_token cancelled);
    std::optional<bake::Point> across(uint32_t edge, Uv target, std::stop_token cancelled) const;
    std::vector<Triangle> triangles;
    std::vector<uint32_t> joins; // directed cross-chart edges requiring unfolded padding

  private:
    double m_inverseScale;
  };
  // Both triangles retain their real geometry. Only this temporary coordinate
  // frame unfolds around their shared world edge for the surface walk.
  std::optional<Coordinates> unfold(const Triangle &from, const Coordinates &coordinates, uint32_t edge,
                                    const Triangle &to);
  double edge(Uv a, Uv b, Uv p);
  std::optional<std::array<double, 3>> barycentric(const Coordinates &uv, Uv p);
  double dot(Point a, Point b);
  Point subtract(Point a, Point b);
  Point cross(Point a, Point b);
  double length(Point a);
  Point unit(Point a);
} // namespace remix::forward::lightmap::seam
