#include "scene/resources.h"
#include "scene/images.h"
#include "scene/hash_stream.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace remix {
  using namespace scene;
  using namespace scene::detail;

  // A sanity bound only: surfaces are indexed with 32 bits throughout, the
  // byte budget below bounds memory, and BLAS builds check the device's
  // maxGeometryCount (2^24 on current drivers).
  constexpr uint32_t maxMeshSurfaces = 65536;

  namespace {
    template <class T> bool samePayload(const std::vector<T> &a, const std::vector<T> &b) {
      return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(T)));
    }
    bool sameSurface(const Surface &a, const Surface &b) {
      return a.material == b.material && a.bonesPerVertex == b.bonesPerVertex &&
          samePayload(a.vertices, b.vertices) && a.indices == b.indices && a.weights == b.weights &&
          a.bones == b.bones && samePayload(a.sprite, b.sprite);
    }
  }
  std::shared_ptr<Surface> NativeResources::shareSurface(std::shared_ptr<Surface> surface) {
    // Hosts may rebuild identical transient geometry under different handles.
    // Compare once here, so all renderer consumers see the same immutable data.
    // Weak, bounded entries cannot keep destroyed geometry or mesh lineage alive.
    const size_t bytes = surface->vertices.size() * sizeof(Vertex) + surface->indices.size() * sizeof(uint32_t) +
        surface->weights.size() * sizeof(float) + surface->bones.size() * sizeof(uint32_t) +
        surface->sprite.size() * sizeof(SpriteVertex);
    if (bytes > 1024 * 1024) return surface;
    HashStream hash;
    hash.append(&surface->material, sizeof(surface->material));
    hash.append(&surface->bonesPerVertex, sizeof(surface->bonesPerVertex));
    auto append = [&](const auto &values) {
      const size_t size = values.size();
      hash.append(&size, sizeof(size));
      hash.append(values.data(), values.size() * sizeof(values[0]));
    };
    append(surface->vertices); append(surface->indices); append(surface->weights);
    append(surface->bones); append(surface->sprite);
    const auto key = hash.digest();
    if (auto found = m_surfaceContent.find(key); found != m_surfaceContent.end()) {
      // A fingerprint is only a lookup hint: collisions never alias geometry.
      if (auto previous = found->second.lock(); previous && sameSurface(*previous, *surface)) return previous;
      found->second = surface;
    } else {
      if (m_surfaceContent.size() == 512) m_surfaceContent.erase(m_surfaceContent.begin());
      m_surfaceContent.emplace(key, surface);
    }
    return surface;
  }

  void NativeResources::createMesh(const remixapi_MeshInfo &info, remixapi_MeshHandle &handle) {
    if (!info.hash || !info.surfaces_count || info.surfaces_count > maxMeshSurfaces || !info.surfaces_values) {
      throw invalid;
    }
    const auto *update = static_cast<const remixapi_MeshInfoUpdateEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT));
    if (update && update->enabled > 1) throw invalid;
    const auto *motion = static_cast<const remixapi_MeshInfoMotionEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT));
    if (motion && (motion->enabled > 1 || (motion->enabled && (!update || !update->enabled)))) throw invalid;
    const auto *sprites = static_cast<const remixapi_MeshInfoSpriteVerticesEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_SPRITE_VERTICES_EXT));
    if (sprites && sprites->enabled > 1) throw invalid;
    const bool spriteVertices = sprites && sprites->enabled;
    auto mesh = std::make_shared<Mesh>();
    mesh->motionCorrespondence = motion && motion->enabled;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < info.surfaces_count; ++i) {
      const auto &source = info.surfaces_values[i];
      if (!source.vertices_values || !source.vertices_count || source.vertices_count > 8 * 1024 * 1024 ||
          source.indices_count > 24 * 1024 * 1024 || (source.indices_count && !source.indices_values) ||
          (source.indices_count ? source.indices_count : source.vertices_count) % 3) {
        throw invalid;
      }
      if (source.material && m_materials.find(key(source.material)) == m_materials.end()) {
        throw invalid;
      }
      bytes += source.vertices_count * sizeof(Vertex) +
               (source.indices_count ? source.indices_count : source.vertices_count) * 4;
      if (bytes > 512u * 1024 * 1024) {
        throw invalid;
      }
      auto surface = std::make_shared<Surface>();
      surface->material = source.material;
      surface->vertices.resize(size_t(source.vertices_count));
      if (spriteVertices)
        surface->sprite.resize(size_t(source.vertices_count));
      std::array<float, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
      for (size_t v = 0; v < surface->vertices.size(); ++v) {
        const auto &src = source.vertices_values[v];
        finite(src.position, 3);
        finite(src.texcoord, 2);
        auto &dst = surface->vertices[v];
        std::copy_n(src.position, 3, dst.position);
        std::copy_n(src.texcoord, 2, dst.uv);
        // Legacy callers may omit normals. Invalid/zero normals retain flat
        // shading; a valid authored normal must survive scene ownership.
        if (spriteVertices) {
          // remixapi_MeshInfoSpriteVerticesEXT: sprite parameters, no normal.
          auto &sprite = surface->sprite[v];
          std::copy_n(src.normal, 3, sprite.frame);
          const uint32_t pads[5] = {src._pad0, src._pad1, src._pad2, src._pad3, src._pad4};
          float values[5];
          std::memcpy(values, pads, sizeof(values));
          std::copy_n(values, 4, sprite.sequence);
          sprite.weight2 = values[4];
          finite(sprite.frame, 3);
          finite(values, 5);
        } else if (std::isfinite(src.normal[0]) && std::isfinite(src.normal[1]) && std::isfinite(src.normal[2])) {
          const float length = std::hypot(src.normal[0], src.normal[1], src.normal[2]);
          if (length > 1e-12f)
            for (int c = 0; c < 3; ++c) dst.normal[c] = src.normal[c] / length;
        }
        rgba(src.color, dst.color);
        for (int c = 0; c < 3; ++c) {
          low[c] = std::min(low[c], src.position[c]);
          high[c] = std::max(high[c], src.position[c]);
        }
      }
      for (int c = 0; c < 3; ++c) {
        surface->center[c] = low[c] * .5f + high[c] * .5f;
      }
      if (source.indices_count) {
        surface->indices.assign(source.indices_values, source.indices_values + source.indices_count);
        for (auto index : surface->indices) {
          if (index >= source.vertices_count) {
            throw invalid;
          }
        }
      } else {
        surface->indices.resize(size_t(source.vertices_count));
        std::iota(surface->indices.begin(), surface->indices.end(), 0u);
      }
      if (source.skinning_hasvalue) {
        const auto &skin = source.skinning_value;
        const uint64_t count = source.vertices_count * skin.bonesPerVertex;
        if (!skin.bonesPerVertex || skin.bonesPerVertex > 256 || count != skin.blendWeights_count ||
            count != skin.blendIndices_count || !skin.blendWeights_values || !skin.blendIndices_values) {
          throw invalid;
        }
        bytes += count * 8;
        if (bytes > 512u * 1024 * 1024) {
          throw invalid;
        }
        surface->bonesPerVertex = skin.bonesPerVertex;
        surface->weights.assign(skin.blendWeights_values, skin.blendWeights_values + count);
        surface->bones.assign(skin.blendIndices_values, skin.blendIndices_values + count);
        for (size_t n = 0; n < count; ++n) {
          if (surface->bones[n] >= 256 || !std::isfinite(surface->weights[n]) || surface->weights[n] < 0) {
            throw invalid;
          }
        }
      }
      mesh->surfaces.push_back(shareSurface(std::move(surface)));
    }
    auto previous = m_meshes.find(info.hash);
    if (update && update->enabled && std::none_of(mesh->surfaces.begin(), mesh->surfaces.end(),
                                                 [](const auto &s) { return s->bonesPerVertex != 0; })) {
      bool compatible = previous != m_meshes.end() && previous->second->updateStream &&
                        previous->second->motionCorrespondence == mesh->motionCorrespondence &&
                        previous->second->surfaces.size() == mesh->surfaces.size();
      if (compatible) for (size_t i = 0; i < mesh->surfaces.size(); ++i) {
        const auto &old = *previous->second->surfaces[i], &next = *mesh->surfaces[i];
        if (old.vertices.size() != next.vertices.size() || old.indices != next.indices) { compatible = false; break; }
      }
      mesh->updateStream = compatible ? previous->second->updateStream : std::make_shared<MeshUpdateStream>();
    }
    // remixapi_MeshInfoDeformEXT (behind "Refresh mesh geometry in place"):
    // new vertex data for the mesh registered under this
    // hash, same topology. Ignored when nothing is registered (a plain create).
    // A different surface count is a different mesh, not a morph: the
    // registered mesh stays and the call still succeeds.
    // Queued draws keep the immutable previous version; the new one joins its
    // update lineage, so the tracer refits the BLAS it already has (skinned
    // meshes included) and temporal history treats it as the same surface.
    if (m_refreshGeometry && previous != m_meshes.end() &&
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_DEFORM_EXT)) {
      const auto &old = *previous->second;
      if (old.surfaces.size() != mesh->surfaces.size()) {
        handle = reinterpret_cast<remixapi_MeshHandle>(uintptr_t(info.hash));
        return;
      }
      // Exact topology keeps the lineage; anything else starts a new one (one
      // full build) instead of refitting a BLAS built for other triangles.
      bool compatible = bool(old.updateStream);
      for (size_t i = 0; compatible && i < mesh->surfaces.size(); ++i) {
        const auto &a = *old.surfaces[i], &b = *mesh->surfaces[i];
        compatible = a.vertices.size() == b.vertices.size() && a.indices == b.indices &&
                     a.bonesPerVertex == b.bonesPerVertex;
      }
      mesh->updateStream = compatible ? old.updateStream : std::make_shared<MeshUpdateStream>();
      mesh->motionCorrespondence = true;
    }
    auto equal = [&](const Mesh &a, const Mesh &b) {
      if (a.updateStream != b.updateStream || a.motionCorrespondence != b.motionCorrespondence) return false;
      if (a.surfaces.size() != b.surfaces.size())
        return false;
      for (size_t i = 0; i < a.surfaces.size(); ++i) {
        const auto &x = *a.surfaces[i], &y = *b.surfaces[i];
        if (!sameSurface(x, y))
          return false;
      }
      return true;
    };
    if (previous == m_meshes.end() || !equal(*previous->second, *mesh)) {
      if (previous != m_meshes.end()) {
        previous->second->retired = true;
        ++m_retireGeneration;
      }
      m_meshes[info.hash] = std::move(mesh);
      ++m_resourceGeneration;
    }
    handle = reinterpret_cast<remixapi_MeshHandle>(uintptr_t(info.hash));
  }

  void NativeResources::destroyMesh(remixapi_MeshHandle handle) {
    if (auto it = m_meshes.find(key(handle)); it != m_meshes.end()) {
      it->second->retired = true;
      ++m_retireGeneration;
      m_meshes.erase(it);
    }
  }
} // namespace remix
