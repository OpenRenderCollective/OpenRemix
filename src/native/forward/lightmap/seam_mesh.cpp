#include "forward/lighting.h"
#include "forward/lightmap/surface_walk.h"
#include "forward/material.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>

namespace remix::forward::lightmap::seam {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Curved seam topology superseded");
    }
    struct Bounds {
      Point minimum{INFINITY, INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY, -INFINITY};
      void add(Point p) {
        for (uint32_t c = 0; c < 3; ++c) {
          minimum[c] = std::min(minimum[c], p[c]);
          maximum[c] = std::max(maximum[c], p[c]);
        }
      }
      bool overlaps(const Bounds &other) const {
        for (uint32_t c = 0; c < 3; ++c)
          if (minimum[c] > other.maximum[c] + .0001 || maximum[c] < other.minimum[c] - .0001)
            return false;
        return true;
      }
    };
    // This private edge hierarchy avoids comparing every pair of chart borders.
    // Its leaves contain original edge IDs; no renderer/GPU state is involved.
    class EdgeIndex {
      struct Node {
        Bounds bounds;
        uint32_t first, count, left = 0, right = 0;
      };
      std::vector<Node> nodes;
      std::vector<uint32_t> order;
      const std::vector<Bounds> &bounds;
      uint32_t build(uint32_t first, uint32_t count, std::stop_token cancelled) {
        check(cancelled);
        const auto index = uint32_t(nodes.size());
        Node node{{}, first, count};
        for (uint32_t i = first; i < first + count; ++i) {
          node.bounds.add(bounds[order[i]].minimum);
          node.bounds.add(bounds[order[i]].maximum);
        }
        nodes.push_back(node);
        if (count > 8) {
          uint32_t axis = 0;
          for (uint32_t c = 1; c < 3; ++c)
            if (node.bounds.maximum[c] - node.bounds.minimum[c] >
                node.bounds.maximum[axis] - node.bounds.minimum[axis])
              axis = c;
          const uint32_t half = count / 2;
          std::nth_element(order.begin() + first, order.begin() + first + half, order.begin() + first + count,
                           [&](uint32_t a, uint32_t b) {
                             return bounds[a].minimum[axis] + bounds[a].maximum[axis] <
                                    bounds[b].minimum[axis] + bounds[b].maximum[axis];
                           });
          const auto left = build(first, half, cancelled),
                     right = build(first + half, count - half, cancelled);
          nodes[index].left = left;
          nodes[index].right = right;
          nodes[index].count = 0;
        }
        return index;
      }

    public:
      EdgeIndex(const std::vector<Bounds> &edges, std::stop_token cancelled) : bounds(edges) {
        order.resize(edges.size());
        std::iota(order.begin(), order.end(), 0u);
        if (!order.empty())
          build(0, uint32_t(order.size()), cancelled);
      }
      void query(const Bounds &box, std::vector<uint32_t> &found) const {
        found.clear();
        if (nodes.empty())
          return;
        std::vector<uint32_t> stack{0};
        while (!stack.empty()) {
          const auto node = nodes[stack.back()];
          stack.pop_back();
          if (!node.bounds.overlaps(box))
            continue;
          if (!node.count) {
            stack.push_back(node.right);
            stack.push_back(node.left);
          } else
            for (uint32_t i = node.first; i < node.first + node.count; ++i)
              if (bounds[order[i]].overlaps(box))
                found.push_back(order[i]);
        }
      }
    };
    bool joinable(const Triangle &a, uint32_t sideA, const Triangle &b, uint32_t sideB) {
      if (dot(a.facing, b.facing) < .5)
        return false;
      const auto start = a.positions[sideA], direction = subtract(a.positions[(sideA + 1) % 3], start);
      const double distance = length(direction);
      if (distance <= .0001)
        return false;
      const auto tangent = unit(direction);
      double lower = INFINITY, upper = -INFINITY;
      for (auto p : {b.positions[sideB], b.positions[(sideB + 1) % 3]}) {
        const auto relative = subtract(p, start);
        const double along = dot(relative, tangent);
        Point radial;
        for (uint32_t c = 0; c < 3; ++c)
          radial[c] = relative[c] - along * tangent[c];
        if (length(radial) > .0001)
          return false;
        lower = std::min(lower, along);
        upper = std::max(upper, along);
      }
      if (std::min(upper, distance) - std::max(lower, 0.) <= .0001)
        return false;
      auto inward = [&](const Triangle &triangle, uint32_t side) {
        auto result = subtract(triangle.positions[(side + 2) % 3], start);
        const double along = dot(result, tangent);
        for (uint32_t c = 0; c < 3; ++c)
          result[c] -= along * tangent[c];
        return unit(result);
      };
      // Reject stacked/overlapping faces, opposite surfaces and sharp corners.
      return dot(inward(a, sideA), inward(b, sideB)) < -.5;
    }
  } // namespace
  SurfaceWalk::SurfaceWalk(const scene::BakeScene &scene, const Layout &layout, std::stop_token cancelled)
      : m_inverseScale(1. / scene.sceneScale) {
    // Vertex identity includes authored seams, like curved parameterization.
    std::map<std::array<uint64_t, 4>, uint32_t> vertices;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
    for (uint32_t chartIndex = 0; chartIndex < scene.charts.size(); ++chartIndex) {
      check(cancelled);
      const auto &chart = scene.charts[chartIndex];
      const auto &draw = scene.draws[chart.draw];
      const auto normalTransform = normalMatrix(draw.model);
      for (size_t first = 0; first < chart.texcoords.size(); first += 3) {
        if ((first & 1023u) == 0)
          check(cancelled);
        Triangle triangle;
        triangle.chart = chartIndex;
        std::array<uint32_t, 3> ids;
        for (uint32_t c = 0; c < 3; ++c) {
          const auto index =
              uint64_t(int64_t(draw.surface->indices[chart.firstIndex + first + c]) + draw.vertexOffset);
          const auto authored = chart.texcoords[first + c];
          const std::array<uint64_t, 4> key{chartIndex, index, std::bit_cast<uint32_t>(authored[0]),
                                            std::bit_cast<uint32_t>(authored[1])};
          ids[c] = vertices.try_emplace(key, uint32_t(vertices.size())).first->second;
          const auto &vertex = draw.surface->vertices[size_t(index)];
          const auto position =
              transformPoint(draw.model, {vertex.position[0], vertex.position[1], vertex.position[2]});
          const auto normal =
              transformVector(normalTransform, {vertex.normal[0], vertex.normal[1], vertex.normal[2]});
          for (uint32_t axis = 0; axis < 3; ++axis) {
            triangle.positions[c][axis] = double(position[axis]) * scene.sceneScale;
            triangle.normals[c][axis] = normal[axis];
          }
          const auto uv = layout.mappings[chartIndex].texcoords[first + c];
          triangle.uv[c] = {uv[0], uv[1]};
        }
        triangle.facing = unit(cross(subtract(triangle.positions[1], triangle.positions[0]),
                                     subtract(triangle.positions[2], triangle.positions[0])));
        Point average{};
        for (auto &normal : triangle.normals) {
          if (length(normal) <= 1e-10)
            normal = triangle.facing;
          normal = unit(normal);
          for (uint32_t c = 0; c < 3; ++c)
            average[c] += normal[c];
        }
        if (dot(average, triangle.facing) < 0)
          for (auto &c : triangle.facing)
            c = -c;
        if (length(triangle.facing) <= 1e-10 || !barycentric(triangle.uv, triangle.uv[0]))
          continue;
        const uint32_t triangleId = uint32_t(triangles.size());
        triangles.push_back(std::move(triangle));
        for (uint32_t side = 0; side < 3; ++side) {
          auto a = ids[side], b = ids[(side + 1) % 3];
          if (a > b)
            std::swap(a, b);
          edges[{a, b}].push_back(triangleId * 3 + side);
        }
      }
    }
    std::vector<uint32_t> borders;
    std::vector<Bounds> borderBounds;
    auto connect = [&](uint32_t a, uint32_t b) {
      triangles[a / 3].neighbours[a % 3].push_back(b);
      triangles[b / 3].neighbours[b % 3].push_back(a);
    };
    for (const auto &[key, occurrences] : edges) {
      if (occurrences.size() == 2)
        connect(occurrences[0], occurrences[1]);
      else if (occurrences.size() == 1) {
        const auto id = occurrences[0];
        const auto &triangle = triangles[id / 3];
        Bounds box;
        box.add(triangle.positions[id % 3]);
        box.add(triangle.positions[(id % 3 + 1) % 3]);
        borders.push_back(id);
        borderBounds.push_back(box);
      }
      // More than two incidents is nonmanifold: keep it closed to walks.
    }
    EdgeIndex index(borderBounds, cancelled);
    std::vector<uint32_t> candidates;
    for (uint32_t first = 0; first < borders.size(); ++first) {
      check(cancelled);
      const auto a = borders[first];
      const auto &ta = triangles[a / 3];
      index.query(borderBounds[first], candidates);
      for (auto second : candidates) {
        if (second <= first)
          continue;
        const auto b = borders[second];
        const auto &tb = triangles[b / 3];
        if (ta.chart == tb.chart)
          continue;
        if (joinable(ta, a % 3, tb, b % 3)) {
          connect(a, b);
          if (layout.mappings[ta.chart].curvedMetric || layout.mappings[tb.chart].curvedMetric ||
              layout.mappings[ta.chart].normalKey != layout.mappings[tb.chart].normalKey) {
            joins.push_back(a);
            joins.push_back(b);
          }
        }
      }
    }
    std::sort(joins.begin(), joins.end());
    joins.erase(std::unique(joins.begin(), joins.end()), joins.end());
  }
} // namespace remix::forward::lightmap::seam
