#include "forward/gpu.h"
#include "forward/material.h"
#include <algorithm>

namespace remix::forward {
void Renderer::Gpu::retireGeometry() {
  constexpr uint64_t budget = 8 * 1024 * 1024;
  constexpr size_t maximumEntries = 32;
  for (auto it = geometries.begin(); it != geometries.end();) {
    auto &geometry = it->second;
    if (!geometry->source.expired() || geometry.use_count() != 1) {
      ++it;
      continue;
    }
    const uint64_t bytes = geometry->vertices->allocationBytes +
                           geometry->indices->allocationBytes;
    if (bytes <= budget) {
      while (!spareGeometry.empty() &&
             (spareGeometryBytes + bytes > budget ||
              spareGeometry.size() == maximumEntries)) {
        const auto &oldest = spareGeometry.front();
        spareGeometryBytes -= oldest->vertices->allocationBytes +
                              oldest->indices->allocationBytes;
        spareGeometry.erase(spareGeometry.begin());
      }
      spareGeometryBytes += bytes;
      spareGeometry.push_back(std::move(geometry));
    }
    it = geometries.erase(it);
  }
}

std::shared_ptr<Renderer::Gpu::Geometry>
Renderer::Gpu::takeGeometry(size_t vertexBytes, size_t indexBytes) {
  auto best = spareGeometry.end();
  uint64_t smallest = UINT64_MAX;
  for (auto it = spareGeometry.begin(); it != spareGeometry.end(); ++it) {
    const auto &geometry = **it;
    const auto bytes =
        geometry.vertices->allocationBytes + geometry.indices->allocationBytes;
    if (geometry.vertices->capacity >= vertexBytes &&
        geometry.indices->capacity >= indexBytes && bytes < smallest) {
      best = it;
      smallest = bytes;
    }
  }
  if (best == spareGeometry.end()) {
    ++statistics.rigidUploadCreates;
    return std::make_shared<Geometry>();
  }
  ++statistics.rigidUploadReuses;
  auto result = std::move(*best);
  spareGeometry.erase(best);
  spareGeometryBytes -= smallest;
  return result;
}

std::shared_ptr<Renderer::Gpu::Geometry>
Renderer::Gpu::geometry(const scene::Draw &draw) {
  const auto &surface = draw.surface;
  const bool skinned = surface->bonesPerVertex != 0;
  if (!skinned)
    if (auto found = geometries.find(surface.get());
        found != geometries.end() && !found->second->source.expired())
      return found->second;
  std::shared_ptr<Geometry> result;
  bool sameSurface = false;
  if (skinned) {
    auto &frame = frames[slot];
    if (frame.posedUsed == frame.posedGeometry.size())
      frame.posedGeometry.push_back(std::make_shared<Geometry>());
    result = frame.posedGeometry[frame.posedUsed++];
    // The slot's fence has completed. Like the path tracer, retain an exact
    // unchanged pose instead of skinning and uploading it again.
    sameSurface = result->source.lock() == surface;
    if (sameSurface && result->bones == *draw.bones) {
      ++statistics.posedUploadReuses;
      return result;
    }
    ++statistics.posedUploads;
  } else {
    result = takeGeometry(surface->vertices.size() * sizeof(scene::Vertex),
                          surface->indices.size() * sizeof(uint32_t));
  }
  result->bounds = {};
  std::vector<scene::Vertex> vertices;
  auto input = std::span<const scene::Vertex>(surface->vertices);
  if (skinned) {
    vertices = surface->vertices;
    if (normalPoseSource != draw.bones.get()) {
      poseNormals.clear();
      poseNormals.reserve(draw.bones->size());
      for (const auto &bone : *draw.bones)
        poseNormals.push_back(normalMatrix(bone));
      normalPoseSource = draw.bones.get();
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
      auto &vertex = vertices[i];
      std::fill_n(vertex.position, 3, 0.f);
      std::fill_n(vertex.normal, 3, 0.f);
      const auto &original = surface->vertices[i];
      for (uint32_t b = 0; b < surface->bonesPerVertex; ++b) {
        const size_t index = i * surface->bonesPerVertex + b;
        const uint32_t bone = surface->bones[index];
        const auto p = transformPoint(
            (*draw.bones)[bone],
            {original.position[0], original.position[1], original.position[2]});
        const auto n = transformVector(
            poseNormals[bone],
            {original.normal[0], original.normal[1], original.normal[2]});
        for (int c = 0; c < 3; ++c) {
          vertex.position[c] += p[c] * surface->weights[index];
          vertex.normal[c] += n[c] * surface->weights[index];
        }
      }
      result->bounds.include({vertex.position[0], vertex.position[1], vertex.position[2]});
    }
    input = vertices;
  } else {
    for (const auto &vertex : input)
      result->bounds.include({vertex.position[0], vertex.position[1], vertex.position[2]});
  }
  ensure(result->vertices, input.size_bytes(),
         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  result->vertices->write(input.data(), input.size_bytes());
  // Posing moves vertices only. The frame slot already owns this surface's
  // index data until it is reused for a different immutable surface.
  if (!sameSurface) {
    ensure(result->indices, surface->indices.size() * sizeof(uint32_t),
           VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    result->indices->write(surface->indices.data(), surface->indices.size() * sizeof(uint32_t));
  }
  result->source = surface;
  if (skinned)
    result->bones = *draw.bones;
  else
    geometries[surface.get()] = result;
  return result;
}
} // namespace remix::forward
