#include "forward/lightmap/decal.h"
#include "forward/lighting.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace {
    Vec3 subtract(Vec3 a, Vec3 b) { for (uint32_t c = 0; c < 3; ++c) a[c] -= b[c]; return a; }
    struct Triangle { std::array<const Vertex *, 3> vertices; std::array<Vec3, 3> world; Bounds bounds; };
    struct Node { Bounds bounds; uint32_t first, count, left = 0, right = 0; };
    bool overlap(const Bounds &a, const Bounds &b, float reach) {
      for (uint32_t c = 0; c < 3; ++c)
        if (a.minimum[c] > b.maximum[c] + reach || b.minimum[c] > a.maximum[c] + reach) return false;
      return true;
    }
    struct ReceiverTree {
      std::vector<Triangle> triangles;
      std::vector<Node> nodes;
      uint32_t build(uint32_t first, uint32_t count) {
        const auto index = uint32_t(nodes.size());
        Node node{{}, first, count};
        for (uint32_t i = first; i < first + count; ++i) node.bounds.merge(triangles[i].bounds);
        nodes.push_back(node);
        if (count > 8) {
          uint32_t axis = 0;
          for (uint32_t c = 1; c < 3; ++c)
            if (node.bounds.maximum[c] - node.bounds.minimum[c] >
                node.bounds.maximum[axis] - node.bounds.minimum[axis]) axis = c;
          const auto middle = first + count / 2;
          std::nth_element(triangles.begin() + first, triangles.begin() + middle,
              triangles.begin() + first + count, [axis](const auto &a, const auto &b) {
                return a.bounds.minimum[axis] + a.bounds.maximum[axis] <
                       b.bounds.minimum[axis] + b.bounds.maximum[axis];
              });
          const auto left = build(first, middle - first), right = build(middle, first + count - middle);
          nodes[index].left = left; nodes[index].right = right; nodes[index].count = 0;
        }
        return index;
      }
    };
    struct Corner { scene::Vertex surface; Vec3 world; };
    Corner interpolate(const Corner &a, const Corner &b, float t) {
      Corner out{};
      for (uint32_t c = 0; c < 3; ++c) {
        out.surface.position[c] = std::lerp(a.surface.position[c], b.surface.position[c], t);
        out.surface.normal[c] = std::lerp(a.surface.normal[c], b.surface.normal[c], t);
        out.world[c] = std::lerp(a.world[c], b.world[c], t);
      }
      for (uint32_t c = 0; c < 2; ++c) out.surface.uv[c] = std::lerp(a.surface.uv[c], b.surface.uv[c], t);
      for (uint32_t c = 0; c < 4; ++c) out.surface.color[c] = std::lerp(a.surface.color[c], b.surface.color[c], t);
      return out;
    }
    void project(const Triangle &receiver, const std::array<Corner, 3> &paint, Mesh &mesh, float reach) {
      const auto e0 = subtract(receiver.world[1], receiver.world[0]),
                 e1 = subtract(receiver.world[2], receiver.world[0]);
      const auto normal = normalized(cross(e0, e1));
      if (dot(normal, normal) < .5f) return;
      // Paint must face its receiver and stay close to that plane. This rejects
      // nearby perpendicular walls and the reverse face of thin geometry.
      const auto paintNormal = normalized(cross(subtract(paint[1].world, paint[0].world),
                                                 subtract(paint[2].world, paint[0].world)));
      if (dot(normal, paintNormal) < .9f) return;
      for (const auto &p : paint)
        if (std::abs(dot(subtract(p.world, receiver.world[0]), normal)) > reach) return;
      std::vector<Corner> polygon(paint.begin(), paint.end());
      for (uint32_t edge = 0; edge < 3 && !polygon.empty(); ++edge) {
        const auto &a = receiver.world[edge], &b = receiver.world[(edge + 1) % 3];
        const auto inward = cross(normal, subtract(b, a));
        std::vector<Corner> clipped;
        auto previous = polygon.back();
        float before = dot(subtract(previous.world, a), inward);
        for (const auto &current : polygon) {
          const float after = dot(subtract(current.world, a), inward);
          if ((before >= 0) != (after >= 0)) clipped.push_back(interpolate(previous, current, before / (before - after)));
          if (after >= 0) clipped.push_back(current);
          previous = current; before = after;
        }
        polygon = std::move(clipped);
      }
      if (polygon.size() < 3) return;
      const float a = dot(e0, e0), b = dot(e0, e1), c = dot(e1, e1), determinant = a * c - b * b;
      if (determinant <= 1e-20f) return;
      auto vertex = [&](const Corner &point) {
        const auto delta = subtract(point.world, receiver.world[0]);
        const float d = dot(delta, e0), e = dot(delta, e1);
        const float v = (c * d - b * e) / determinant, w = (a * e - b * d) / determinant;
        Vertex out = *receiver.vertices[0];
        out.surface = point.surface;
        for (uint32_t component = 0; component < 2; ++component)
          out.coordinate[component] = receiver.vertices[0]->coordinate[component] * (1 - v - w) +
              receiver.vertices[1]->coordinate[component] * v + receiver.vertices[2]->coordinate[component] * w;
        return out;
      };
      for (uint32_t i = 1; i + 1 < polygon.size(); ++i) {
        if (dot(cross(subtract(polygon[i].world, polygon[0].world), subtract(polygon[i + 1].world, polygon[0].world)), normal) <= 1e-12f)
          continue;
        for (uint32_t corner : {0u, i, i + 1}) {
          mesh.indices.push_back(uint32_t(mesh.vertices.size()));
          mesh.vertices.push_back(vertex(polygon[corner]));
        }
      }
    }
  }
  std::vector<Mesh> decalMeshes(const scene::BakeScene &source, std::span<const Mesh> receivers,
                                std::stop_token cancelled) {
    if (std::none_of(source.draws.begin(), source.draws.end(), [](const auto &draw) {
          return !draw.terrain && acceptsSurfaceLayer(draw);
        })) return {};
    // Refer to immutable input vertices rather than copying their atlas/material
    // attributes into every acceleration-tree triangle.
    ReceiverTree tree;
    for (const auto &mesh : receivers) {
      const auto &draw = source.draws.at(mesh.draw);
      if (!accepts(draw)) continue;
      for (uint32_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        if (cancelled.stop_requested()) throw std::runtime_error("Decal receiver projection superseded");
        Triangle triangle;
        for (uint32_t c = 0; c < 3; ++c) {
          triangle.vertices[c] = &mesh.vertices.at(mesh.indices[i + c]);
          const auto &p = triangle.vertices[c]->surface.position;
          triangle.world[c] = transformPoint(draw.model, {p[0], p[1], p[2]});
          triangle.bounds.include(triangle.world[c]);
        }
        tree.triangles.push_back(triangle);
      }
    }
    if (tree.triangles.empty()) return {};
    tree.build(0, uint32_t(tree.triangles.size()));
    std::vector<Mesh> result;
    const float reach = .005f / source.sceneScale;
    for (uint32_t index = 0; index < source.draws.size(); ++index) {
      if (cancelled.stop_requested()) throw std::runtime_error("Decal receiver projection superseded");
      const auto &draw = source.draws[index];
      if (draw.terrain || !acceptsSurfaceLayer(draw)) continue;
      Mesh mesh; mesh.draw = index;
      const auto &surface = *draw.surface;
      const uint32_t count = draw.indexCount ? draw.indexCount : uint32_t(surface.indices.size());
      for (uint32_t i = 0; i + 2 < count; i += 3) {
        if (cancelled.stop_requested()) throw std::runtime_error("Decal receiver projection superseded");
        std::array<Corner, 3> paint;
        Bounds bounds;
        for (uint32_t c = 0; c < 3; ++c) {
          paint[c].surface = surface.vertices.at(surface.indices.at(draw.firstIndex + i + c) + draw.vertexOffset);
          const auto &p = paint[c].surface.position;
          paint[c].world = transformPoint(draw.model, {p[0], p[1], p[2]});
          bounds.include(paint[c].world);
        }
        std::vector<uint32_t> pending{0};
        while (!pending.empty()) {
          const auto &node = tree.nodes[pending.back()]; pending.pop_back();
          if (!overlap(bounds, node.bounds, reach)) continue;
          if (node.count)
            for (uint32_t j = node.first; j < node.first + node.count; ++j) project(tree.triangles[j], paint, mesh, reach);
          else { pending.push_back(node.left); pending.push_back(node.right); }
        }
      }
      if (!mesh.indices.empty()) result.push_back(std::move(mesh));
    }
    return result;
  }
}
