#include "forward/lightmap/surface_walk.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::lightmap::seam {
  double dot(Point a, Point b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
  Point subtract(Point a, Point b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
  Point cross(Point a, Point b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  }
  double length(Point a) { return std::hypot(a[0], a[1], a[2]); }
  Point unit(Point a) {
    const double scale = length(a);
    if (scale <= 1e-20)
      return {};
    for (auto &v : a)
      v /= scale;
    return a;
  }
  double edge(Uv a, Uv b, Uv p) { return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]); }
  std::optional<std::array<double, 3>> barycentric(const Coordinates &uv, Uv p) {
    const double area = edge(uv[0], uv[1], uv[2]);
    if (std::abs(area) <= 1e-14)
      return std::nullopt;
    return std::array{edge(uv[1], uv[2], p) / area, edge(uv[2], uv[0], p) / area,
                      edge(uv[0], uv[1], p) / area};
  }
  std::optional<Coordinates> unfold(const Triangle &from, const Coordinates &uv, uint32_t side,
                                    const Triangle &to) {
    const auto a = side, b = (side + 1) % 3, c = (side + 2) % 3;
    const auto origin = from.positions[a], ab = subtract(from.positions[b], origin),
               ac = subtract(from.positions[c], origin);
    const double width = length(ab);
    if (width <= .0001)
      return std::nullopt;
    const auto tangent = unit(ab);
    const double along = dot(ac, tangent);
    Point perpendicular;
    for (uint32_t axis = 0; axis < 3; ++axis)
      perpendicular[axis] = ac[axis] - along * tangent[axis];
    const double height = length(perpendicular);
    if (height <= 1e-10)
      return std::nullopt;
    Coordinates result;
    for (uint32_t i = 0; i < 3; ++i) {
      const auto relative = subtract(to.positions[i], origin);
      const double x = dot(relative, tangent);
      Point radial;
      for (uint32_t axis = 0; axis < 3; ++axis)
        radial[axis] = relative[axis] - x * tangent[axis];
      // The adjacent triangle occupies the other side of the hinge. This also
      // handles a shorter or longer collinear edge at a T-junction.
      const double y = -length(radial);
      for (uint32_t axis = 0; axis < 2; ++axis) {
        const double horizontal = (uv[b][axis] - uv[a][axis]) / width;
        const double vertical = (uv[c][axis] - uv[a][axis] - along * horizontal) / height;
        result[i][axis] = uv[a][axis] + x * horizontal + y * vertical;
      }
    }
    if (!barycentric(result, result[0]))
      return std::nullopt;
    return result;
  }
  namespace {
    bool containsEdgePoint(const Triangle &triangle, uint32_t side, Point point) {
      const auto a = triangle.positions[side], direction = subtract(triangle.positions[(side + 1) % 3], a);
      const double squared = dot(direction, direction), distance = dot(subtract(point, a), direction);
      return squared > 0 && distance >= -.0001 * std::sqrt(squared) &&
             distance <= squared + .0001 * std::sqrt(squared);
    }
  } // namespace
  std::optional<bake::Point> SurfaceWalk::across(uint32_t seed, Uv target, std::stop_token cancelled) const {
    const auto &receiver = triangles[seed / 3];
    const auto side = seed % 3, next = (side + 1) % 3;
    const auto ownWeights = barycentric(receiver.uv, target);
    if (!ownWeights || (*ownWeights)[(side + 2) % 3] >= -1e-8)
      return std::nullopt;
    const Uv startEdge = receiver.uv[side], endEdge = receiver.uv[next];
    const Uv direction{endEdge[0] - startEdge[0], endEdge[1] - startEdge[1]};
    const double denominator = direction[0] * direction[0] + direction[1] * direction[1];
    if (denominator <= 1e-20)
      return std::nullopt;
    const double t = std::clamp(
        ((target[0] - startEdge[0]) * direction[0] + (target[1] - startEdge[1]) * direction[1]) / denominator,
        1e-7, 1 - 1e-7);
    const Uv start{startEdge[0] + t * direction[0], startEdge[1] + t * direction[1]};
    Point contact;
    for (uint32_t c = 0; c < 3; ++c)
      contact[c] = receiver.positions[side][c] * (1 - t) + receiver.positions[next][c] * t;
    uint32_t current = seed / 3, crossed = side;
    Coordinates coordinates = receiver.uv;
    double progress = 0;
    // Each step must advance along the same straight segment in the unfolded
    // surface. A walk ends at an open edge; connectivity elsewhere cannot jump
    // across a hole. The bound protects planning from pathological micro-meshes.
    for (uint32_t step = 0; step < 256; ++step) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Curved seam walk superseded");
      std::optional<Coordinates> unfolded;
      uint32_t neighbour = unowned;
      for (auto candidate : triangles[current].neighbours[crossed]) {
        if (!containsEdgePoint(triangles[candidate / 3], candidate % 3, contact))
          continue;
        auto frame = unfold(triangles[current], coordinates, crossed, triangles[candidate / 3]);
        if (!frame)
          continue;
        const auto weights = barycentric(*frame, {start[0] * (1 - progress) + target[0] * progress,
                                                  start[1] * (1 - progress) + target[1] * progress});
        if (!weights || *std::min_element(weights->begin(), weights->end()) < -1e-6)
          continue;
        unfolded = std::move(frame);
        neighbour = candidate / 3;
        break;
      }
      if (!unfolded)
        return std::nullopt;
      current = neighbour;
      coordinates = *unfolded;
      const auto &triangle = triangles[current];
      if (dot(receiver.facing, triangle.facing) < .5)
        return std::nullopt;
      const auto targetWeights = barycentric(coordinates, target),
                 startWeights = barycentric(coordinates, start);
      if (!targetWeights || !startWeights)
        return std::nullopt;
      if (*std::min_element(targetWeights->begin(), targetWeights->end()) >= -1e-8) {
        bake::Point point;
        Point normal{};
        for (uint32_t i = 0; i < 3; ++i)
          for (uint32_t c = 0; c < 3; ++c) {
            point.position[c] += float(triangle.positions[i][c] * (*targetWeights)[i] * m_inverseScale);
            normal[c] += triangle.normals[i][c] * (*targetWeights)[i];
          }
        normal = unit(normal);
        if (length(normal) == 0)
          normal = triangle.facing;
        for (uint32_t c = 0; c < 3; ++c)
          point.normal[c] = float(normal[c]);
        return point;
      }
      double exit = 2;
      uint32_t opposite = 0;
      for (uint32_t i = 0; i < 3; ++i) {
        if ((*targetWeights)[i] >= -1e-8)
          continue;
        const double delta = (*targetWeights)[i] - (*startWeights)[i];
        if (delta >= -1e-14)
          continue;
        const double at = -(*startWeights)[i] / delta;
        if (at > progress + 1e-9 && at < exit) {
          exit = at;
          opposite = i;
        }
      }
      if (exit > 1)
        return std::nullopt;
      progress = exit;
      crossed = (opposite + 1) % 3;
      const Uv at{start[0] * (1 - progress) + target[0] * progress,
                  start[1] * (1 - progress) + target[1] * progress};
      const auto weights = *barycentric(coordinates, at);
      contact = {};
      for (uint32_t i = 0; i < 3; ++i)
        for (uint32_t c = 0; c < 3; ++c)
          contact[c] += triangle.positions[i][c] * weights[i];
    }
    return std::nullopt;
  }
} // namespace remix::forward::lightmap::seam
