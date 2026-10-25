#include "forward/lightmap/receiver_triangles.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace {
    template <class Corners> uint32_t firstCorner(const Corners &corners) {
      uint32_t first = 0;
      for (uint32_t i = 1; i < 3; ++i)
        if (corners[i] < corners[first]) first = i;
      return first;
    }
    bool sameMapping(const Vertex &a, const Vertex &b) {
      return a.coordinate == b.coordinate && a.atlasBounds == b.atlasBounds &&
             a.lights == b.lights && a.directLights == b.directLights;
    }
    bool nearPose(const Matrix &a, const Matrix &b, float tolerance) {
      for (uint32_t i = 0; i < 16; ++i) {
        const float limit = i >= 12 && i < 15 ? tolerance : 1e-6f * std::max({1.f, std::abs(a[i]), std::abs(b[i])});
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(a[i] - b[i]) > limit) return false;
      }
      return true;
    }
  }
  ReceiverTriangles::ReceiverTriangles(const Mesh &source, std::stop_token cancelled) : m_draw(source.draw) {
    if (source.indices.size() % 3 || source.indices.size() > maximumIndices)
      throw std::invalid_argument("Receiver triangle directory exceeds its bounded triangle-list budget");
    for (size_t first = 0; first < source.indices.size(); first += 3) {
      if (first % 3072 == 0 && cancelled.stop_requested())
        throw std::runtime_error("Receiver triangle directory superseded");
      Key positions;
      for (uint32_t corner = 0; corner < 3; ++corner) {
        const auto &vertex = source.vertices.at(source.indices[first + corner]);
        std::copy_n(vertex.surface.position, 3, positions[corner].begin());
        for (float value : positions[corner])
          if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite receiver triangle position");
      }
      if (positions[0] == positions[1] || positions[1] == positions[2] || positions[2] == positions[0]) continue;
      const uint32_t start = firstCorner(positions);
      Key key;
      Triangle triangle;
      bool mapped = true;
      for (uint32_t corner = 0; corner < 3; ++corner) {
        key[corner] = positions[(start + corner) % 3];
        triangle.corners[corner] = source.vertices[source.indices[first + (start + corner) % 3]];
        mapped &= triangle.corners[corner].coordinate[3] >= .5f;
      }
      triangle.ambiguous = !mapped;
      auto [found, inserted] = m_triangles.emplace(key, triangle);
      if (!inserted)
        for (uint32_t corner = 0; corner < 3; ++corner)
          found->second.ambiguous |= triangle.ambiguous ||
              !sameMapping(found->second.corners[corner], triangle.corners[corner]);
    }
  }
  std::optional<Mesh> ReceiverTriangles::map(const scene::Draw &captured, const scene::Draw &live,
                                             float sceneScale) const {
    if (!std::isfinite(sceneScale) || sceneScale <= 0 || !live.surface || !live.traced() || live.bones ||
        (live.mesh && live.mesh->updateStream) ||
        live.surface->bonesPerVertex || live.viewModel || live.wireframe || live.decal || live.cutGroup ||
        live.instanceId != captured.instanceId || live.instanceIndex != captured.instanceIndex ||
        live.surfaceIndex != captured.surfaceIndex || live.doubleSided != captured.doubleSided ||
        live.reflectionOnly != captured.reflectionOnly || live.terrain != captured.terrain)
      return {};
    // Source model matrices can carry float roundoff from the engine's vertex
    // shader constants. Bound that allowance in world space as well as matrix
    // space; it must never become a moving-receiver approximation.
    const float tolerance = 1e-5f / sceneScale;
    if (!std::isfinite(tolerance) || !nearPose(captured.model, live.model, tolerance)) return {};
    const auto &surface = *live.surface;
    const size_t count = live.indexCount ? live.indexCount : surface.indices.size();
    if (!count || count % 3 || count > maximumIndices || live.firstIndex > surface.indices.size() ||
        count > surface.indices.size() - live.firstIndex) return {};
    Mesh result;
    result.draw = m_draw;
    result.vertices.reserve(count);
    result.indices.reserve(count);
    for (size_t first = live.firstIndex; first < live.firstIndex + count; first += 3) {
      Key positions;
      std::array<const scene::Vertex *, 3> vertices;
      for (uint32_t corner = 0; corner < 3; ++corner) {
        const int64_t index = int64_t(surface.indices[first + corner]) + live.vertexOffset;
        if (index < 0 || uint64_t(index) >= surface.vertices.size()) return {};
        vertices[corner] = &surface.vertices[size_t(index)];
        std::copy_n(vertices[corner]->position, 3, positions[corner].begin());
        for (float value : positions[corner]) if (!std::isfinite(value)) return {};
      }
      const uint32_t start = firstCorner(positions);
      const Key key{positions[start], positions[(start + 1) % 3], positions[(start + 2) % 3]};
      const auto found = m_triangles.find(key);
      if (found == m_triangles.end() || found->second.ambiguous) return {};
      for (uint32_t corner = 0; corner < 3; ++corner) {
        const auto position = transformPoint(captured.model, positions[corner]);
        const auto actual = transformPoint(live.model, positions[corner]);
        double distance2 = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          if (!std::isfinite(position[axis]) || !std::isfinite(actual[axis])) return {};
          const double delta = double(actual[axis]) - position[axis];
          distance2 += delta * delta;
        }
        if (distance2 > double(tolerance) * tolerance) return {};
        auto vertex = found->second.corners[(corner + 3 - start) % 3];
        vertex.surface = *vertices[corner]; // live UVs, normals and vertex colours
        result.indices.push_back(uint32_t(result.vertices.size()));
        result.vertices.push_back(vertex);
      }
    }
    return result;
  }
}
