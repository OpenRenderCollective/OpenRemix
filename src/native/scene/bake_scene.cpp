#include "scene/bake_scene.h"
#include "scene/bake_material.h"
#include "scene/resources.h"
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <unordered_set>

namespace remix::scene {
  namespace {
    constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    struct Hash {
      uint64_t value = 1469598103934665603ull;
      template <class T> void add(const T &v) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        value = hashBytes(&v, sizeof(v), value);
      }
      template <class T, size_t N> void add(const T (&v)[N]) {
        for (const auto &element : v)
          add(element);
      }
      template <class T, size_t N> void add(const std::array<T, N> &v) {
        for (const auto &element : v)
          add(element);
      }
    };
    struct Freezer {
      std::unordered_map<const Texture *, uint64_t> textures;
      std::unordered_map<const Material *, std::pair<std::shared_ptr<Material>, uint64_t>> materials;
      std::unordered_map<const Surface *, std::pair<std::shared_ptr<Surface>, uint64_t>> surfaces;

      uint64_t texture(const std::shared_ptr<Texture> &source) {
        if (!source)
          return 0;
        if (source->dynamic || source->updates)
          throw invalid;
        if (auto found = textures.find(source.get()); found != textures.end())
          return found->second;
        // Never use a caller's texture hash as a content fingerprint. Do not
        // mutate Texture::contentHash while a rendering thread may read it.
        Hash hash;
        const auto &pixels = source->pixels;
        hash.add(pixels.width);
        hash.add(pixels.height);
        hash.add(pixels.srgb);
        hash.add(pixels.twoChannel);
        hash.add(pixels.levels());
        for (const auto &mip : pixels.mips)
          hash.value = fastHash(mip.data(), mip.size(), hash.value);
        for (const auto &mip : pixels.hdrMips)
          hash.value = fastHash(mip.data(), mip.size() * sizeof(float), hash.value);
        if (pixels.compressed) {
          hash.add(pixels.compressed->format);
          hash.add(pixels.compressed->bc1Alpha);
          hash.add(pixels.compressed->hash);
        }
        textures.emplace(source.get(), hash.value);
        return hash.value;
      }
      std::pair<std::shared_ptr<Material>, uint64_t> material(const std::shared_ptr<Material> &source) {
        if (!source || source->fps || (source->normalEncoding == 5 && !source->bakeSmoothBoundary) ||
            (source->bakeSmoothBoundary && !validSmoothBoundary(*source)))
          throw invalid;
        if (auto found = materials.find(source.get()); found != materials.end())
          return found->second;
        auto result = std::make_shared<Material>();
        Hash hash;
        auto copy = [&]<class T>(T &out, const T &in) {
          if constexpr (std::is_array_v<T>)
            std::copy(std::begin(in), std::end(in), std::begin(out));
          else
            out = in;
          hash.add(in);
        };
        // Copy transport/shading fields explicitly. Mutable emission caches and
        // retirement flags belong to their source resource, never this snapshot.
        result->sourceHash = source->sourceHash;
        auto map = [&](auto &out, const auto &in) {
          out = in;
          hash.add(texture(in));
        };
        map(result->texture, source->texture);
        map(result->emissionTexture, source->emissionTexture);
        map(result->roughnessTexture, source->roughnessTexture);
        map(result->metallicTexture, source->metallicTexture);
        // Mean optical boundaries own no ripple image, including dynamic normal
        // frames. Other maps still pass the ordinary immutability checks.
        if (!source->bakeSmoothBoundary)
          map(result->normalTexture, source->normalTexture);
        else
          hash.add(uint64_t(0));
        copy(result->roughness, source->roughness);
        copy(result->metallic, source->metallic);
        copy(result->pbr, source->pbr);
        copy(result->normalEncoding, source->bakeSmoothBoundary ? 0u : source->normalEncoding);
        copy(result->bakeSmoothBoundary, source->bakeSmoothBoundary);
        for (uint32_t i = 0; i < 2; ++i) {
          auto &out = result->remap[i];
          const auto &in = source->remap[i];
          copy(out.channel, in.channel);
          copy(out.curve, in.curve);
          copy(out.invert, in.invert);
          copy(out.exponent, in.exponent);
          copy(out.bias, in.bias);
          copy(out.scale, in.scale);
        }
        copy(result->refractiveIndex, source->refractiveIndex);
        copy(result->transmittanceDistance, source->transmittanceDistance);
        copy(result->emission, source->emission);
        copy(result->suppressEmissionLight, source->suppressEmissionLight);
        copy(result->selfIllumination, source->selfIllumination);
        copy(result->selfIlluminationScale, source->selfIlluminationScale);
        copy(result->tint, source->tint);
        copy(result->compare, source->compare);
        copy(result->reference, source->reference);
        copy(result->drawAlpha, source->drawAlpha);
        copy(result->translucent, source->translucent);
        copy(result->transmission, source->transmission);
        copy(result->filter, source->filter);
        copy(result->wrapU, source->wrapU);
        copy(result->wrapV, source->wrapV);
        copy(result->rows, source->rows);
        copy(result->columns, source->columns);
        auto entry = std::pair{std::move(result), hash.value};
        materials.emplace(source.get(), entry);
        return entry;
      }
      std::pair<std::shared_ptr<Surface>, uint64_t> surface(const std::shared_ptr<Surface> &source) {
        if (!source || source->bonesPerVertex || !source->sprite.empty())
          throw invalid;
        if (auto found = surfaces.find(source.get()); found != surfaces.end())
          return found->second;
        auto result = std::make_shared<Surface>(*source);
        Hash hash;
        hash.add(uint64_t(result->vertices.size()));
        hash.add(uint64_t(result->indices.size()));
        for (const auto &vertex : result->vertices) {
          hash.add(vertex.position);
          hash.add(vertex.normal);
          hash.add(vertex.uv);
          hash.add(vertex.color);
        }
        for (auto index : result->indices)
          hash.add(index);
        auto entry = std::pair{std::move(result), hash.value};
        surfaces.emplace(source.get(), entry);
        return entry;
      }
    };
    void lightHash(Hash &hash, const SphereLight &light) {
      hash.add(light.position);
      hash.add(light.radius);
      hash.add(light.radiance);
      hash.add(light.triangle);
      hash.add(light.corners);
      hash.add(light.direction);
      hash.add(light.coneCos);
      hash.add(light.softness);
      hash.add(light.focus);
    }
  } // namespace
  void freezeBakeScene(BakeScene &scene) {
    validateBakeCharts(scene.draws, scene.charts);
    std::vector<std::vector<uint64_t>> chartHashes(scene.draws.size());
    for (const auto &chart : scene.charts) {
      Hash hash;
      hash.add(chart.firstIndex);
      hash.add(chart.width);
      hash.add(chart.height);
      hash.add(uint64_t(chart.texcoords.size()));
      for (const auto &uv : chart.texcoords)
        hash.add(uv);
      chartHashes[chart.draw].push_back(hash.value);
    }
    Freezer freezer;
    scene.bindings.clear();
    std::vector<uint64_t> records;
    for (size_t drawIndex = 0; drawIndex < scene.draws.size(); ++drawIndex) {
      auto &draw = scene.draws[drawIndex];
      if (!draw.traced() || draw.classification != REMIXAPI_INSTANCE_STATIC_EXT || draw.bones || !draw.mesh ||
          draw.mesh->updateStream || draw.viewModel || draw.wireframe || draw.cutGroup)
        throw invalid;
      scene.bindings.push_back({draw.surface, draw.material});
      auto [surface, geometryHash] = freezer.surface(draw.surface);
      auto [material, materialHash] = freezer.material(draw.material);
      draw.surface = std::move(surface);
      draw.material = std::move(material);
      // A backend needs only the frozen surface, not a mutable source mesh.
      draw.mesh.reset();
      Hash hash;
      auto &charts = chartHashes[drawIndex];
      std::sort(charts.begin(), charts.end());
      hash.add(uint64_t(charts.size()));
      for (auto chart : charts)
        hash.add(chart);
      hash.add(geometryHash);
      hash.add(materialHash);
      hash.add(draw.model);
      hash.add(draw.doubleSided);
      hash.add(draw.ignoreAlpha);
      hash.add(draw.alphaOverride);
      hash.add(draw.compare);
      hash.add(draw.reference);
      hash.add(draw.firstIndex);
      hash.add(draw.indexCount);
      hash.add(draw.vertexOffset);
      const auto &blend = draw.blend;
      hash.add(blend.enabled);
      hash.add(blend.src);
      hash.add(blend.dst);
      hash.add(blend.op);
      hash.add(blend.srcAlpha);
      hash.add(blend.dstAlpha);
      hash.add(blend.alphaOp);
      hash.add(blend.mask);
      hash.add(blend.colorOp);
      hash.add(blend.color1);
      hash.add(blend.color2);
      hash.add(blend.alphaOpTexture);
      hash.add(blend.alpha1);
      hash.add(blend.alpha2);
      hash.add(blend.factor);
      records.push_back(hash.value);
    }
    for (const auto &light : scene.lights) {
      if (light.dynamic)
        throw invalid;
      Hash hash;
      lightHash(hash, light);
      records.push_back(hash.value);
    }
    // Membership order and runtime handles are not content. Preserve duplicate
    // placements/lights, because multiplicity can change transport.
    std::sort(records.begin(), records.end());
    Hash hash;
    hash.add(uint32_t(5)); // serialized input schema; bump when transport fields change
    hash.add(scene.sceneId);
    hash.add(scene.sceneScale);
    hash.add(scene.hasBounds);
    if (scene.hasBounds) {
      hash.add(scene.minimum);
      hash.add(scene.maximum);
    }
    hash.add(bool(scene.partition));
    if (scene.partition) {
      hash.add(uint64_t(scene.partition->nodes.size()));
      hash.add(uint64_t(scene.partition->leafClusters.size()));
      hash.add(scene.partition->closed);
      hash.add(scene.partition->clusterCount);
      for (const auto &node : scene.partition->nodes) {
        hash.add(node.plane);
        hash.add(node.children);
      }
      for (auto cluster : scene.partition->leafClusters)
        hash.add(cluster);
    }
    hash.add(uint64_t(scene.reflectionHints.size()));
    for (const auto &position : scene.reflectionHints)
      hash.add(position);
    hash.add(uint64_t(scene.draws.size()));
    hash.add(uint64_t(scene.lights.size()));
    for (auto record : records)
      hash.add(record);
    hash.add(scene.sun.direction);
    hash.add(scene.sun.radiance);
    hash.add(scene.sun.angularDiameterDegrees);
    hash.add(scene.sun.isDistant);
    hash.add(scene.dome.enabled);
    hash.add(scene.dome.radiance);
    hash.add(scene.dome.worldToLocal);
    hash.add(freezer.texture(scene.dome.texture));
    scene.contentHash = hash.value ? hash.value : 1;
  }
} // namespace remix::scene

namespace remix {
  bool NativeResources::bakeRequestCurrent(const remixapi_BakeSceneInfoEXT &info) const {
    const bool reflections = info.version == REMIXAPI_BAKE_SCENE_REFLECTIONS_VERSION_EXT;
    const bool surfaces = reflections || info.version == REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT;
    const bool bounded = surfaces || info.version == REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT;
    if (!m_bakeScene || m_bakeScene->hasBounds != bounded || m_bakeScene->partition != m_visibilityPartition)
      return false;
    if (reflections) {
      const auto &request = *reinterpret_cast<const remixapi_BakeSceneReflectionsInfoEXT *>(&info);
      if (m_bakeScene->reflectionRevision != request.reflectionRevision ||
          m_bakeScene->reflectionHints.size() != request.positionCount)
        return false;
    } else if (m_bakeScene->reflectionRevision || !m_bakeScene->reflectionHints.empty()) {
      return false;
    }
    if (surfaces) {
      const auto &request = *reinterpret_cast<const remixapi_BakeSceneSurfacesInfoEXT *>(&info);
      if (m_bakeScene->chartRevision != request.chartRevision ||
          m_bakeScene->charts.size() != request.chartCount)
        return false;
    } else if (!m_bakeScene->charts.empty()) {
      return false;
    }
    if (bounded) {
      const auto &bounds = *reinterpret_cast<const remixapi_BakeSceneBoundsInfoEXT *>(&info);
      if (!std::equal(std::begin(bounds.minimum), std::end(bounds.minimum), m_bakeScene->minimum.begin()) ||
          !std::equal(std::begin(bounds.maximum), std::end(bounds.maximum), m_bakeScene->maximum.begin()))
        return false;
    }
    if (!m_bakeScene || m_bakeScene->sceneId != info.sceneId || m_bakeScene->revision != info.revision ||
        m_bakeScene->sceneScale != info.sceneScale ||
        m_bakeScene->ready != (info.state == REMIXAPI_BAKE_SCENE_READY_EXT) ||
        m_bakeInstances.size() != info.instanceCount || m_bakeLights.size() != info.lightCount)
      return false;
    for (uint32_t i = 0; i < info.instanceCount; ++i) {
      const auto [handle, revision] = m_bakeInstances[i];
      if (handle != info.instances[i])
        return false;
      const auto *entry = retained(handle);
      if (!entry || entry->revision != revision || retainedStale(*entry) || !entry->visible ||
          entry->resolvedModGeneration != m_modGeneration)
        return false;
    }
    for (uint32_t i = 0; i < info.lightCount; ++i) {
      const auto [handle, revision] = m_bakeLights[i];
      if (handle != uint64_t(reinterpret_cast<uintptr_t>(info.lights[i])))
        return false;
      const auto found = m_lights.find(handle);
      if (found == m_lights.end() || found->second.revision != revision)
        return false;
    }
    return true;
  }
  void NativeResources::setBakeScene(const remixapi_BakeSceneInfoEXT *info) {
    constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (!info) {
      m_bakeScene.reset();
      m_bakeInstances.clear();
      m_bakeLights.clear();
      return;
    }
    const bool reflections = info->version == REMIXAPI_BAKE_SCENE_REFLECTIONS_VERSION_EXT &&
                             info->structSize == sizeof(remixapi_BakeSceneReflectionsInfoEXT);
    const auto *reflectionInfo =
        reflections ? reinterpret_cast<const remixapi_BakeSceneReflectionsInfoEXT *>(info) : nullptr;
    if (reflections && (reflectionInfo->reserved || reflectionInfo->positionCount > 4096 ||
                        (reflectionInfo->positionCount &&
                         (!reflectionInfo->positions || !reflectionInfo->reflectionRevision)) ||
                        (info->state == REMIXAPI_BAKE_SCENE_LOADING_EXT && reflectionInfo->positionCount)))
      throw invalid;
    const bool surfaces = reflections || (info->version == REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT &&
                                          info->structSize == sizeof(remixapi_BakeSceneSurfacesInfoEXT));
    const bool bounded = surfaces || (info->version == REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT &&
                                      info->structSize == sizeof(remixapi_BakeSceneBoundsInfoEXT));
    const auto *surfaceInfo =
        surfaces ? reinterpret_cast<const remixapi_BakeSceneSurfacesInfoEXT *>(info) : nullptr;
    if (surfaces && (surfaceInfo->reserved || surfaceInfo->chartCount > (1u << 20) ||
                     (surfaceInfo->chartCount && (!surfaceInfo->charts || !surfaceInfo->chartRevision)) ||
                     (info->state == REMIXAPI_BAKE_SCENE_LOADING_EXT && surfaceInfo->chartCount)))
      throw invalid;
    const bool legacy = info->version == REMIXAPI_BAKE_SCENE_VERSION_EXT && info->structSize == sizeof(*info);
    if ((!bounded && !legacy) || !info->sceneId || !info->revision || !std::isfinite(info->sceneScale) ||
        info->sceneScale <= 0 || info->state > REMIXAPI_BAKE_SCENE_READY_EXT ||
        (info->instanceCount && !info->instances) || (info->lightCount && !info->lights) ||
        (info->state == REMIXAPI_BAKE_SCENE_LOADING_EXT && (info->instanceCount || info->lightCount)))
      throw invalid;
    if (bounded) {
      const auto &bounds = *reinterpret_cast<const remixapi_BakeSceneBoundsInfoEXT *>(info);
      for (uint32_t c = 0; c < 3; ++c)
        if (!std::isfinite(bounds.minimum[c]) || !std::isfinite(bounds.maximum[c]) ||
            bounds.minimum[c] >= bounds.maximum[c])
          throw invalid;
    }
    if (bakeRequestCurrent(*info))
      return;
    auto next = std::make_shared<scene::BakeScene>();
    next->sceneId = info->sceneId;
    next->revision = info->revision;
    next->sceneScale = info->sceneScale;
    next->ready = info->state == REMIXAPI_BAKE_SCENE_READY_EXT;
    next->hasBounds = bounded;
    next->partition = m_visibilityPartition;
    if (bounded) {
      const auto &bounds = *reinterpret_cast<const remixapi_BakeSceneBoundsInfoEXT *>(info);
      std::copy_n(bounds.minimum, 3, next->minimum.begin());
      std::copy_n(bounds.maximum, 3, next->maximum.begin());
    }
    if (reflections) {
      next->reflectionRevision = reflectionInfo->reflectionRevision;
      next->reflectionHints.resize(reflectionInfo->positionCount);
      for (uint32_t i = 0; i < reflectionInfo->positionCount; ++i)
        for (uint32_t c = 0; c < 3; ++c) {
          const float value = reflectionInfo->positions[size_t(i) * 3 + c];
          if (!std::isfinite(value) || value < next->minimum[c] || value > next->maximum[c])
            throw invalid;
          next->reflectionHints[i][c] = value;
        }
    }
    std::vector<std::pair<uint64_t, uint64_t>> instances, lights;
    std::unordered_set<uint64_t> seen;
    std::unordered_map<uint64_t, size_t> drawBases;
    for (uint32_t i = 0; i < info->instanceCount; ++i) {
      const auto *entry = retained(info->instances[i]);
      if (!entry || !seen.insert(entry->handle).second || !entry->mesh || entry->watch || !entry->placed ||
          entry->description.hidden || !entry->visible || retainedStale(*entry) ||
          entry->resolvedModGeneration != m_modGeneration)
        throw invalid;
      drawBases.emplace(entry->handle, next->draws.size());
      instances.emplace_back(entry->handle, entry->revision);
      next->draws.insert(next->draws.end(), entry->records.begin(), entry->records.end());
    }
    if (surfaces) {
      next->chartRevision = surfaceInfo->chartRevision;
      uint64_t totalCorners = 0;
      for (uint32_t i = 0; i < surfaceInfo->chartCount; ++i) {
        const auto &source = surfaceInfo->charts[i];
        const auto base = drawBases.find(source.instance);
        const auto *entry = retained(source.instance);
        totalCorners += source.indexCount;
        if (base == drawBases.end() || !entry || source.surfaceIndex >= entry->surfaceCount ||
            source.instanceIndex >= entry->instanceCount() || !source.indexCount || source.indexCount % 3 ||
            !source.texcoords || totalCorners > (1u << 24))
          throw invalid;
        scene::BakeSurfaceChart chart;
        const size_t draw =
            base->second + size_t(source.instanceIndex) * entry->surfaceCount + source.surfaceIndex;
        if (draw >= next->draws.size())
          throw invalid;
        chart.draw = uint32_t(draw);
        chart.firstIndex = source.firstIndex;
        chart.width = source.width;
        chart.height = source.height;
        chart.texcoords.resize(source.indexCount);
        for (uint32_t corner = 0; corner < source.indexCount; ++corner)
          std::copy_n(source.texcoords + size_t(corner) * 2, 2, chart.texcoords[corner].begin());
        next->charts.push_back(std::move(chart));
      }
    }
    seen.clear();
    for (uint32_t i = 0; i < info->lightCount; ++i) {
      const auto key = uint64_t(reinterpret_cast<uintptr_t>(info->lights[i]));
      const auto found = m_lights.find(key);
      if (found == m_lights.end() || !seen.insert(key).second)
        throw invalid;
      const auto &light = found->second;
      if (light.dynamic)
        throw invalid;
      lights.emplace_back(key, light.revision);
      next->lights.insert(next->lights.end(), light.emitters.begin(), light.emitters.end());
      if (light.distant.isDistant) {
        if (next->sun.isDistant)
          throw invalid; // one sun, never silently overwrite another
        next->sun = light.distant;
      }
      if (light.dome.enabled) {
        if (next->dome.enabled)
          throw invalid;
        next->dome = light.dome;
      }
    }
    if (next->ready)
      scene::freezeBakeScene(*next);
    m_bakeInstances = std::move(instances);
    m_bakeLights = std::move(lights);
    m_bakeScene = std::move(next);
  }
} // namespace remix
