#include "scene/resources.h"
#include "scene/images.h"
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
  // A normal's length: sqrt where the squares cannot overflow or underflow
  // (std::hypot is far slower and only needed outside that range).
  namespace {
    float normalLength(const float *n) {
      const float squares = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
      return squares > 1e-30f && squares < 1e30f ? std::sqrt(squares) : std::hypot(n[0], n[1], n[2]);
    }
  } // namespace

  std::shared_ptr<Surface> NativeResources::prepareSurface(const remixapi_MeshInfoSurfaceTriangles &source,
                                                           bool spriteVertices, uint64_t &bytes) {
    if (!source.vertices_values || !source.vertices_count || source.vertices_count > 8 * 1024 * 1024 ||
        source.indices_count > 24 * 1024 * 1024 || (source.indices_count && !source.indices_values) ||
        (source.indices_count ? source.indices_count : source.vertices_count) % 3) {
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
        const float length = normalLength(src.normal);
        if (length > 1e-12f)
          for (int c = 0; c < 3; ++c)
            dst.normal[c] = src.normal[c] / length;
      }
      rgba(src.color, dst.color);
      for (int c = 0; c < 3; ++c) {
        low[c] = std::min(low[c], src.position[c]);
        high[c] = std::max(high[c], src.position[c]);
      }
    }
    for (int c = 0; c < 3; ++c)
      surface->center[c] = low[c] * .5f + high[c] * .5f;
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
    return surface;
  }

  void NativeResources::adoptMesh(uint64_t hash, std::vector<std::shared_ptr<Surface>> surfaces,
                                  remixapi_MeshHandle &handle) {
    if (!hash || surfaces.empty())
      throw invalid;
    auto mesh = std::make_shared<Mesh>();
    mesh->surfaces = std::move(surfaces);
    if (auto previous = m_meshes.find(hash); previous != m_meshes.end()) {
      previous->second->retired = true;
      ++m_retireGeneration;
    }
    m_meshes[hash] = std::move(mesh);
    ++m_resourceGeneration;
    handle = reinterpret_cast<remixapi_MeshHandle>(uintptr_t(hash));
  }

  void NativeResources::createMesh(const remixapi_MeshInfo &info, remixapi_MeshHandle &handle) {
    if (!info.hash || !info.surfaces_count || info.surfaces_count > maxMeshSurfaces || !info.surfaces_values) {
      throw invalid;
    }
    const auto *update =
        static_cast<const remixapi_MeshInfoUpdateEXT *>(extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT));
    if (update && update->enabled > 1)
      throw invalid;
    const auto *motion =
        static_cast<const remixapi_MeshInfoMotionEXT *>(extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT));
    if (motion && (motion->enabled > 1 || (motion->enabled && (!update || !update->enabled))))
      throw invalid;
    const auto *sprites = static_cast<const remixapi_MeshInfoSpriteVerticesEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_SPRITE_VERTICES_EXT));
    if (sprites && sprites->enabled > 1)
      throw invalid;
    const bool spriteVertices = sprites && sprites->enabled;
    auto mesh = std::make_shared<Mesh>();
    mesh->motionCorrespondence = motion && motion->enabled;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < info.surfaces_count; ++i) {
      const auto &source = info.surfaces_values[i];
      if (source.material && m_materials.find(key(source.material)) == m_materials.end()) {
        throw invalid;
      }
      mesh->surfaces.push_back(prepareSurface(source, spriteVertices, bytes));
    }
    auto previous = m_meshes.find(info.hash);
    if (update && update->enabled && std::none_of(mesh->surfaces.begin(), mesh->surfaces.end(), [](const auto &s) {
          return s->bonesPerVertex != 0;
        })) {
      bool compatible = previous != m_meshes.end() && previous->second->updateStream &&
                        previous->second->motionCorrespondence == mesh->motionCorrespondence &&
                        previous->second->surfaces.size() == mesh->surfaces.size();
      if (compatible)
        for (size_t i = 0; i < mesh->surfaces.size(); ++i) {
          const auto &old = *previous->second->surfaces[i], &next = *mesh->surfaces[i];
          if (old.vertices.size() != next.vertices.size() || old.indices != next.indices) {
            compatible = false;
            break;
          }
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
        compatible =
            a.vertices.size() == b.vertices.size() && a.indices == b.indices && a.bonesPerVertex == b.bonesPerVertex;
      }
      mesh->updateStream = compatible ? old.updateStream : std::make_shared<MeshUpdateStream>();
      mesh->motionCorrespondence = true;
    }
    auto equal = [&](const Mesh &a, const Mesh &b) {
      if (a.updateStream != b.updateStream || a.motionCorrespondence != b.motionCorrespondence)
        return false;
      if (a.surfaces.size() != b.surfaces.size())
        return false;
      for (size_t i = 0; i < a.surfaces.size(); ++i) {
        const auto &x = *a.surfaces[i], &y = *b.surfaces[i];
        if (x.material != y.material || x.vertices.size() != y.vertices.size() || x.indices != y.indices ||
            x.weights != y.weights || x.bones != y.bones || x.bonesPerVertex != y.bonesPerVertex ||
            std::memcmp(x.vertices.data(), y.vertices.data(), x.vertices.size() * sizeof(Vertex)) ||
            x.sprite.size() != y.sprite.size() ||
            std::memcmp(x.sprite.data(), y.sprite.data(), x.sprite.size() * sizeof(SpriteVertex)))
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

  void NativeResources::updateMeshVertices(remixapi_MeshHandle handle, uint32_t count,
                                           const remixapi_MeshVerticesEXT *updates) {
    const auto previous = m_meshes.find(key(handle));
    if (previous == m_meshes.end() || !count || !updates)
      throw invalid;
    const auto &old = *previous->second;
    if (count > old.surfaces.size())
      throw invalid;
    // Every check first: an invalid call changes nothing.
    std::vector<bool> seen(old.surfaces.size());
    for (uint32_t i = 0; i < count; ++i) {
      const auto &u = updates[i];
      if (u.surface >= old.surfaces.size() || seen[u.surface] || !u.positions ||
          u.vertexCount != old.surfaces[u.surface]->vertices.size())
        throw invalid;
      seen[u.surface] = true;
      finite(u.positions, size_t(u.vertexCount) * 3);
      if (u.texcoords)
        finite(u.texcoords, size_t(u.vertexCount) * 2);
    }
    auto mesh = std::make_shared<Mesh>();
    mesh->surfaces = old.surfaces; // Unchanged surfaces are shared: they are immutable.
    // Same topology by construction: the lineage continues, as MeshInfoDeformEXT's would.
    if (m_refreshGeometry) {
      mesh->updateStream = old.updateStream ? old.updateStream : std::make_shared<MeshUpdateStream>();
      mesh->motionCorrespondence = true;
    }
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) {
      const auto &u = updates[i];
      auto surface = std::make_shared<Surface>(*old.surfaces[u.surface]);
      std::array<float, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
      for (size_t v = 0; v < surface->vertices.size(); ++v) {
        auto &dst = surface->vertices[v];
        const float *position = u.positions + v * 3;
        std::copy_n(position, 3, dst.position);
        for (int c = 0; c < 3; ++c) {
          low[c] = std::min(low[c], position[c]);
          high[c] = std::max(high[c], position[c]);
        }
        if (u.texcoords)
          std::copy_n(u.texcoords + v * 2, 2, dst.uv);
        if (u.normals) {
          const float *n = u.normals + v * 3;
          std::fill_n(dst.normal, 3, 0.f);
          if (std::isfinite(n[0]) && std::isfinite(n[1]) && std::isfinite(n[2])) {
            const float length = normalLength(n);
            if (length > 1e-12f)
              for (int c = 0; c < 3; ++c)
                dst.normal[c] = n[c] / length;
          }
        }
      }
      for (int c = 0; c < 3; ++c)
        surface->center[c] = low[c] * .5f + high[c] * .5f;
      const auto &before = old.surfaces[u.surface]->vertices;
      changed |= std::memcmp(before.data(), surface->vertices.data(), before.size() * sizeof(Vertex)) != 0;
      mesh->surfaces[u.surface] = std::move(surface);
    }
    // The same vertices again: nothing changes (as for CreateMesh).
    if (!changed)
      return;
    previous->second->retired = true;
    ++m_retireGeneration;
    previous->second = std::move(mesh);
    ++m_resourceGeneration;
  }

  void NativeResources::destroyMesh(remixapi_MeshHandle handle) {
    if (auto it = m_meshes.find(key(handle)); it != m_meshes.end()) {
      it->second->retired = true;
      ++m_retireGeneration;
      m_meshes.erase(it);
    }
  }
} // namespace remix
