#include "forward/lightmap/surface_metric.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>

namespace remix::forward::lightmap::curved {
  namespace {
    using Row = std::vector<std::pair<uint32_t, double>>;
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Curved surface mapping superseded");
    }
    // A positive-weight Dirichlet Laplacian is symmetric positive definite when
    // every interior component reaches the fixed boundary. Solve one coordinate
    // with Jacobi-preconditioned conjugate gradients, without a dense matrix.
    bool solve(const std::vector<Row> &rows, const std::vector<uint8_t> &boundary, std::vector<Uv> &uv,
               uint32_t axis, std::stop_token cancelled) {
      const size_t count = rows.size();
      std::vector<double> diagonal(count), x(count), rhs(count), residual(count), direction(count),
          product(count), preconditioned(count);
      for (size_t i = 0; i < count; ++i) {
        if (boundary[i])
          continue;
        for (auto [j, weight] : rows[i]) {
          diagonal[i] += weight;
          if (boundary[j])
            rhs[i] += weight * uv[j][axis];
        }
        if (!(diagonal[i] > 0))
          return false;
        residual[i] = rhs[i];
        direction[i] = preconditioned[i] = residual[i] / diagonal[i];
      }
      auto inner = [](const auto &a, const auto &b) {
        return std::inner_product(a.begin(), a.end(), b.begin(), 0.);
      };
      const double tolerance = std::max(1e-28, inner(rhs, rhs) * 1e-20);
      double previous = inner(residual, preconditioned);
      for (uint32_t iteration = 0; iteration < 2048; ++iteration) {
        if ((iteration & 15u) == 0)
          check(cancelled);
        if (inner(residual, residual) <= tolerance) {
          for (size_t i = 0; i < count; ++i)
            if (!boundary[i])
              uv[i][axis] = x[i];
          return true;
        }
        for (size_t i = 0; i < count; ++i) {
          if (boundary[i])
            continue;
          product[i] = diagonal[i] * direction[i];
          for (auto [j, weight] : rows[i])
            if (!boundary[j])
              product[i] -= weight * direction[j];
        }
        const double denominator = inner(direction, product);
        if (!(denominator > 0) || !std::isfinite(previous))
          return false;
        const double alpha = previous / denominator;
        for (size_t i = 0; i < count; ++i) {
          if (boundary[i])
            continue;
          x[i] += alpha * direction[i];
          residual[i] -= alpha * product[i];
          preconditioned[i] = residual[i] / diagonal[i];
        }
        const double next = inner(residual, preconditioned), beta = next / previous;
        for (size_t i = 0; i < count; ++i)
          direction[i] = preconditioned[i] + beta * direction[i];
        previous = next;
      }
      return false;
    }
  } // namespace
  std::optional<std::vector<Uv>> harmonic(const Surface &surface, std::stop_token cancelled) {
    check(cancelled);
    const auto count = surface.positions.size();
    // Source displacement grids are small. Larger or non-disk charts retain
    // their authored topology and still get the measured metric correction.
    if (count < 4 || count > 8192 || surface.corners.size() > 196608)
      return std::nullopt;
    struct Edge {
      uint32_t count = 0;
      double weight = 0;
    };
    std::map<std::pair<uint32_t, uint32_t>, Edge> edges;
    for (size_t first = 0; first < surface.corners.size(); first += 3) {
      if ((first & 1023u) == 0)
        check(cancelled);
      for (uint32_t corner = 0; corner < 3; ++corner) {
        auto a = surface.corners[first + corner], b = surface.corners[first + (corner + 1) % 3];
        const auto opposite = surface.corners[first + (corner + 2) % 3];
        if (a == b || a == opposite || b == opposite)
          return std::nullopt;
        const auto u = difference(surface.positions[a], surface.positions[opposite]),
                   v = difference(surface.positions[b], surface.positions[opposite]);
        const double area = length(cross(u, v));
        if (area <= 1e-20)
          return std::nullopt;
        if (a > b)
          std::swap(a, b);
        auto &edge = edges[{a, b}];
        if (++edge.count > 2)
          return std::nullopt;
        edge.weight += dot(u, v) / area;
      }
    }
    if (int64_t(count) - int64_t(edges.size()) + int64_t(surface.corners.size() / 3) != 1)
      return std::nullopt;
    std::vector<Row> rows(count);
    std::vector<std::vector<uint32_t>> border(count);
    for (const auto &[indices, edge] : edges) {
      const auto [a, b] = indices;
      // Clamping negative cotangents keeps the convex-combination property on
      // obtuse triangles. A strict positive floor also keeps the graph connected.
      const double weight = std::clamp(edge.weight, 1e-8, 1e8);
      rows[a].emplace_back(b, weight);
      rows[b].emplace_back(a, weight);
      if (edge.count == 1) {
        border[a].push_back(b);
        border[b].push_back(a);
      }
    }
    std::vector<uint8_t> boundary(count), visited(count);
    uint32_t start = uint32_t(count), boundaryCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (border[i].empty())
        continue;
      if (border[i].size() != 2)
        return std::nullopt;
      start = std::min(start, i);
      boundary[i] = 1;
      ++boundaryCount;
    }
    if (start == count)
      return std::nullopt;
    std::vector<uint32_t> loop;
    uint32_t previous = start, current = start;
    do {
      if (visited[current])
        return std::nullopt;
      visited[current] = 1;
      loop.push_back(current);
      const auto next = border[current][0] == previous ? border[current][1] : border[current][0];
      previous = current;
      current = next;
    } while (current != start);
    if (loop.size() != boundaryCount)
      return std::nullopt;
    // A disconnected closed component could otherwise escape the boundary walk.
    std::vector<uint32_t> reached{start};
    std::fill(visited.begin(), visited.end(), 0);
    visited[start] = 1;
    for (size_t i = 0; i < reached.size(); ++i)
      for (auto [j, weight] : rows[reached[i]])
        if (!visited[j]) {
          visited[j] = 1;
          reached.push_back(j);
        }
    if (reached.size() != count)
      return std::nullopt;
    std::vector<size_t> corners;
    for (size_t i = 0; i < loop.size(); ++i) {
      const auto a = surface.authored[loop[(i + loop.size() - 1) % loop.size()]],
                 b = surface.authored[loop[i]], c = surface.authored[loop[(i + 1) % loop.size()]];
      const double ux = b[0] - a[0], uy = b[1] - a[1], vx = c[0] - b[0], vy = c[1] - b[1];
      const double lengths = std::hypot(ux, uy) * std::hypot(vx, vy);
      if (lengths <= 1e-20 || ux * vx + uy * vy < 0 || std::abs(ux * vy - uy * vx) > lengths * 1e-4)
        corners.push_back(i);
    }
    if (corners.size() != 4)
      return std::nullopt;
    const auto offset = corners.front();
    std::rotate(loop.begin(), loop.begin() + offset, loop.end());
    for (auto &corner : corners)
      corner -= offset;
    std::array<double, 4> lengths{};
    for (uint32_t side = 0; side < 4; ++side) {
      const size_t end = side == 3 ? loop.size() : corners[side + 1];
      for (size_t i = corners[side]; i < end; ++i)
        lengths[side] +=
            length(difference(surface.positions[loop[(i + 1) % loop.size()]], surface.positions[loop[i]]));
      if (lengths[side] <= 1e-10)
        return std::nullopt;
    }
    const double width = (lengths[0] + lengths[2]) * .5, height = (lengths[1] + lengths[3]) * .5;
    const Uv rectangle[] = {{0, 0}, {width, 0}, {width, height}, {0, height}, {0, 0}};
    std::vector<Uv> uv(count);
    for (uint32_t side = 0; side < 4; ++side) {
      const size_t end = side == 3 ? loop.size() : corners[side + 1];
      double along = 0;
      for (size_t i = corners[side]; i < end; ++i) {
        const double t = along / lengths[side];
        for (uint32_t axis = 0; axis < 2; ++axis)
          uv[loop[i]][axis] = rectangle[side][axis] * (1 - t) + rectangle[side + 1][axis] * t;
        along +=
            length(difference(surface.positions[loop[(i + 1) % loop.size()]], surface.positions[loop[i]]));
      }
    }
    if (!solve(rows, boundary, uv, 0, cancelled) || !solve(rows, boundary, uv, 1, cancelled))
      return std::nullopt;
    double orientation = 0;
    for (size_t first = 0; first < surface.corners.size(); first += 3) {
      const auto a = uv[surface.corners[first]], b = uv[surface.corners[first + 1]],
                 c = uv[surface.corners[first + 2]];
      const double area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
      if (!std::isfinite(area) || std::abs(area) < 1e-14 * width * height || area * orientation < 0)
        return std::nullopt;
      orientation = area;
    }
    return uv;
  }
} // namespace remix::forward::lightmap::curved
