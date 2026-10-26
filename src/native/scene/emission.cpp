#include "scene/emission.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <mutex>

namespace remix::scene {
  namespace {
    // Texture channels have only 256 possible inputs. Animated texture snapshots
    // invalidate the reduction cache, but not this conversion or the quadrature.
    const auto linearChannels = [] {
      std::array<std::array<float, 256>, 2> values{};
      for (unsigned i = 0; i < 256; ++i) {
        const float value = i / 255.f;
        values[0][i] = value;
        values[1][i] = std::pow(value, 2.2f); // sRGB-encoded: gamma 2.2
      }
      return values;
    }();
    const auto emissionSamples = [] {
      std::array<std::array<float, 3>, 256> weights{};
      for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
          const float a = std::sqrt((x + .5f) / 16), b = (y + .5f) / 16;
          weights[y * 16 + x] = {1 - a, a * (1 - b), a * b};
        }
      return weights;
    }();
    // The dynamic textures (remixapi_TextureInfoDynamicEXT) a material's
    // emission reads, locked: their texels change under these locks.
    struct DynamicTextures {
      std::unique_lock<std::mutex> locks[2];
      uint64_t revision = 0; // sum of the textures' revisions
      bool any = false;
      explicit DynamicTextures(const Material &m) {
        TextureUpdates *updates[2] = {
            m.emissionTexture && m.emissionTexture->dynamic ? m.emissionTexture->updates.get() : nullptr,
            m.selfIllumination && m.texture && m.texture->dynamic ? m.texture->updates.get() : nullptr};
        if (updates[0] == updates[1])
          updates[1] = nullptr;
        if (updates[1] && (!updates[0] || std::less<>()(updates[1], updates[0])))
          std::swap(updates[0], updates[1]); // one lock order
        for (unsigned i = 0; i < 2; ++i)
          if (updates[i]) {
            locks[i] = std::unique_lock(updates[i]->lock);
            revision += updates[i]->revision;
            any = true;
          }
      }
    };
    // A triangle's emission; `cached`: through Material::emissionCache. The
    // caller holds the locks of the material's dynamic textures.
    std::array<float, 3> emission(const Draw &draw, uint32_t triangle, const float *transform, bool cached = true);
  } // namespace
  std::shared_ptr<const SurfaceEmissionCache::Reduction>
  SurfaceEmissionCache::reduce(const Draw &draw, const std::array<float, 4> &uv) {
    const auto &m = *draw.material;
    // Self-illumination also reads the albedo: its texture and tint are part
    // of the entry (the scale is positive, so zero means "off").
    const Key key{{m.emission[0], m.emission[1], m.emission[2]},
                  uv,
                  m.wrapU,
                  m.wrapV,
                  m.rows,
                  m.columns,
                  m.fps,
                  m.selfIllumination,
                  {m.selfIllumination ? m.selfIlluminationScale : 0, m.selfIllumination ? m.tint[0] : 0,
                   m.selfIllumination ? m.tint[1] : 0, m.selfIllumination ? m.tint[2] : 0}};
    const std::shared_ptr<Texture> albedo = m.selfIllumination ? m.texture : nullptr;
    // Dynamic textures: the material's estimates (Material::emissionCache)
    // belong to one image. Once the pixels changed and those estimates are old
    // enough they are dropped; the surfaces' reductions follow.
    const DynamicTextures dynamic(m);
    const auto now = dynamic.any ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (dynamic.any &&
        (!m.emissionSerial || (m.emissionRevision != dynamic.revision &&
                               std::chrono::duration<double>(now - m.emissionMade).count() >= m.emissionKeepSeconds))) {
      m.emissionCache.clear();
      m.emissionRevision = dynamic.revision;
      m.emissionMade = now;
      m.emissionKeepSeconds = dynamicEmissionSeconds;
      ++m.emissionSerial;
    }
    auto &cache = surfaces[draw.surface.get()];
    if (cache.owner.lock() != draw.surface)
      cache = {draw.surface, {}};
    bool refresh = false;
    if (const auto found = cache.entries.find(key); found != cache.entries.end()) {
      const auto &entry = found->second;
      if (entry.textured == bool(m.emissionTexture) && entry.texture.lock() == m.emissionTexture &&
          entry.colored == bool(albedo) && entry.albedo.lock() == albedo) {
        if (!dynamic.any || entry.serial == m.emissionSerial)
          return entry.reduction;
        refresh = true;
      }
    }
    auto result = std::make_shared<Reduction>();
    for (uint32_t triangle = 0; triangle < draw.surface->indices.size() / 3; ++triangle) {
      const auto radiance = emission(draw, triangle, uv.data());
      if (radiance[0] > 0 || radiance[1] > 0 || radiance[2] > 0)
        result->push_back({triangle, radiance, m.emissionTexture ? emissionHistoryKey(draw, triangle) : 0});
    }
    if (dynamic.any) {
      // What this cost bounds how soon the material's estimates are remade.
      const auto spent = std::chrono::steady_clock::now() - now;
      m.emissionKeepSeconds = std::max(m.emissionKeepSeconds, 100 * std::chrono::duration<double>(spent).count());
      if (refresh) {
        textureCounters.emissionRefreshes.fetch_add(1, std::memory_order_relaxed);
        textureCounters.emissionRefreshTicks.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count()), std::memory_order_relaxed);
      }
    }
    // Bound sprite/setting variants without retaining old surfaces or textures.
    if (cache.entries.size() >= 8)
      cache.entries.clear();
    cache.entries.insert_or_assign(
        key, Entry{m.emissionTexture, bool(m.emissionTexture), result, albedo, bool(albedo), m.emissionSerial});
    return result;
  }

  void SurfaceEmissionCache::prune() {
    std::erase_if(surfaces, [](const auto &entry) { return entry.second.owner.expired(); });
    std::erase_if(emitterEntries, [&](const auto &entry) { return entry.second.used != emitterFrame; });
    ++emitterFrame;
  }

  void triangleEmitters(const Draw &draw, const SurfaceEmissionCache::Reduction &reduction,
                        std::vector<SphereLight> &out) {
    out.clear();
    out.reserve(reduction.size());
    for (const auto &triangle : reduction)
      out.push_back(triangleEmitter(draw, triangle.index, triangle.radiance));
    if (out.size() < 2)
      return;
    const auto &s = *draw.surface;
    std::vector<std::array<float, 3>> centers(reduction.size());
    float low[3], high[3];
    std::fill_n(low, 3, std::numeric_limits<float>::max());
    std::fill_n(high, 3, std::numeric_limits<float>::lowest());
    for (size_t i = 0; i < reduction.size(); ++i) {
      auto &center = centers[i];
      center = {};
      for (unsigned v = 0; v < 3; ++v) {
        const auto &p = s.vertices[s.indices[reduction[i].index * 3 + v]].position;
        for (unsigned c = 0; c < 3; ++c)
          center[c] += p[c] / 3;
      }
      for (unsigned c = 0; c < 3; ++c) {
        low[c] = std::min(low[c], center[c]);
        high[c] = std::max(high[c], center[c]);
      }
    }
    // 10 bits an axis, interleaved.
    const auto spread = [](uint32_t x) {
      x &= 1023;
      x = (x | x << 16) & 0x030000FF;
      x = (x | x << 8) & 0x0300F00F;
      x = (x | x << 4) & 0x030C30C3;
      return (x | x << 2) & 0x09249249;
    };
    std::vector<std::pair<uint32_t, uint32_t>> codes(reduction.size());
    for (size_t i = 0; i < reduction.size(); ++i) {
      uint32_t code = 0;
      for (unsigned c = 0; c < 3; ++c) {
        const float extent = high[c] - low[c];
        const auto cell = uint32_t(extent > 0 ? std::min(1023.f, (centers[i][c] - low[c]) / extent * 1024) : 0);
        code |= spread(cell) << c;
      }
      codes[i] = {code, uint32_t(i)};
    }
    std::sort(codes.begin(), codes.end());
    for (uint32_t r = 0; r < codes.size(); ++r)
      out[codes[r].second].rank = r;
  }

  std::shared_ptr<const std::vector<SphereLight>>
  SurfaceEmissionCache::emitters(const Draw &draw, const std::shared_ptr<const Reduction> &reduction) {
    // A list a frame may still hold is replaced, not changed.
    const auto make = [&](std::shared_ptr<std::vector<SphereLight>> &lights) {
      if (!lights || lights.use_count() > 1)
        lights = std::make_shared<std::vector<SphereLight>>();
      triangleEmitters(draw, *reduction, *lights);
    };
    if (draw.surface->bonesPerVertex && draw.bones) {
      std::shared_ptr<std::vector<SphereLight>> lights;
      make(lights);
      return lights;
    }
    const Surface *surface = draw.surface.get();
    const uint64_t identity[] = {draw.instanceId, uint64_t(draw.instanceIndex) << 32 | draw.surfaceIndex,
                                 uint64_t(reinterpret_cast<uintptr_t>(surface))};
    auto &entry = emitterEntries[hashBytes(identity, sizeof(identity))];
    if (entry.reduction != reduction || entry.surface.lock().get() != surface || entry.model != draw.model ||
        entry.instanceId != draw.instanceId || entry.instanceIndex != draw.instanceIndex ||
        entry.surfaceIndex != draw.surfaceIndex || entry.doubleSided != draw.doubleSided ||
        (!draw.instanceId && draw.mesh)) {
      entry.reduction = reduction;
      entry.surface = draw.surface;
      entry.model = draw.model;
      entry.instanceId = draw.instanceId;
      entry.instanceIndex = draw.instanceIndex;
      entry.surfaceIndex = draw.surfaceIndex;
      entry.doubleSided = draw.doubleSided;
      make(entry.lights);
    }
    entry.used = emitterFrame;
    return entry.lights;
  }

  uint64_t emissionHistoryKey(const Draw &draw, uint32_t triangle) {
    const auto &m = *draw.material;
    std::array<uint8_t, 64> bytes{};
    size_t size = 0;
    auto add = [&](const auto &value) {
      auto p = reinterpret_cast<const uint8_t *>(&value);
      std::copy_n(p, sizeof(value), bytes.data() + size);
      size += sizeof(value);
    };
    add(m.emission);
    add(m.wrapU);
    add(m.wrapV);
    add(m.rows);
    add(m.columns);
    add(m.fps);
    if (m.emissionTexture) {
      add(m.emissionTexture->pixels.width);
      add(m.emissionTexture->pixels.height);
      add(m.emissionTexture->pixels.srgb);
    }
    for (unsigned i = 0; i < 3; ++i)
      add(draw.surface->vertices[draw.surface->indices[triangle * 3 + i]].uv);
    return hashBytes(bytes.data(), size);
  }
  void EmissionHistory::begin(uint64_t nextWorld, double seconds, uint32_t nextMode) {
    if (nextWorld != world || nextMode != mode || seconds < time)
      entries.clear();
    alpha = -std::expm1(-std::max(0., seconds - time) / .5);
    world = nextWorld;
    time = seconds;
    mode = nextMode;
    ++sequence;
    rawChange = filteredChange = 0;
  }
  void EmissionHistory::apply(SphereLight &light, uint64_t layout) {
    if (!mode || !light.triangle)
      return;
    if (!(light.radiance[0] > 0 || light.radiance[1] > 0 || light.radiance[2] > 0)) {
      entries.erase(light.id);
      return;
    }
    // World-space geometry changes invalidate correspondence independently of
    // animation. Hash only initialized xyz corners, never structure padding.
    layout ^= hashBytes(light.corners, sizeof(light.corners));
    auto [it, inserted] = entries.try_emplace(light.id);
    auto &entry = it->second;
    if (inserted || entry.layout != layout)
      for (unsigned c = 0; c < 3; ++c)
        entry.raw[c] = entry.filtered[c] = light.radiance[c];
    entry.layout = layout;
    entry.seen = sequence;
    for (unsigned c = 0; c < 3; ++c) {
      const float raw = light.radiance[c], previous = entry.filtered[c];
      rawChange += std::abs(raw - entry.raw[c]);
      if (mode == 1) {
        entry.filtered[c] += float(alpha * (double(raw) - entry.filtered[c]));
        if (std::abs(entry.filtered[c] - raw) <= 1e-5f * std::max(1.f, raw))
          entry.filtered[c] = raw;
      }
      filteredChange += std::abs(entry.filtered[c] - previous);
      entry.raw[c] = raw;
      light.radiance[c] = entry.filtered[c];
    }
  }
  void EmissionHistory::end() {
    std::erase_if(entries, [&](const auto &item) { return item.second.seen != sequence; });
  }
  std::array<float, 3> triangleEmission(const Draw &draw, uint32_t triangle, const float *transform) {
    // A dynamic texture's texels are read under its lock, and its estimates
    // are kept only by SurfaceEmissionCache::reduce, which ages them.
    const auto &m = *draw.material;
    const DynamicTextures dynamic(m);
    return emission(draw, triangle, transform, !dynamic.any);
  }
  namespace {
  std::array<float, 3> emission(const Draw &draw, uint32_t triangle, const float *transform, bool cached) {
    const auto &m = *draw.material;
    if (m.emission[0] == 0 && m.emission[1] == 0 && m.emission[2] == 0)
      return {};
    std::array<float, 3> result{};
    if (!m.emissionTexture && !m.selfIllumination) {
      std::copy_n(m.emission, 3, result.begin());
      return result;
    }
    const auto &s = *draw.surface;
    float uv[3][2];
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned c = 0; c < 2; ++c)
        uv[i][c] = s.vertices[s.indices[triangle * 3 + i]].uv[c] * transform[c] + transform[c + 2];
    const auto key = cached ? hashBytes(uv, sizeof(uv)) : 0;
    if (cached)
      if (auto it = m.emissionCache.find(key); it != m.emissionCache.end())
        return it->second;
    // Compressed textures keep no decoded mip 0; their <=64x64 preview level is
    // an area-filtered stand-in for these stratified triangle averages.
    struct Map {
      uint32_t width = 0, height = 0;
      const uint8_t *texels = nullptr;
      const float *hdrTexels = nullptr;
      const std::array<float, 256> *channels = nullptr;
      explicit operator bool() const { return width && height && (texels || hdrTexels); }
      // Linear RGB and stored alpha.
      std::array<float, 4> at(size_t index) const {
        if (hdrTexels)
          return {hdrTexels[index], hdrTexels[index + 1], hdrTexels[index + 2], hdrTexels[index + 3]};
        return {(*channels)[texels[index]], (*channels)[texels[index + 1]], (*channels)[texels[index + 2]],
                texels[index + 3] / 255.f};
      }
    };
    auto open = [](const Texture *texture) {
      Map map;
      if (!texture)
        return map;
      const auto &p = texture->pixels;
      const auto *compressed = p.compressed.get();
      map.width = compressed ? compressed->previewWidth : p.width;
      map.height = compressed ? compressed->previewHeight : p.height;
      map.texels = compressed ? (compressed->preview.empty() ? nullptr : compressed->preview.data())
                              : p.mips.empty() ? nullptr : p.mips[0].data();
      map.hdrTexels = compressed ? (compressed->hdrPreview.empty() ? nullptr : compressed->hdrPreview.data())
                                 : (p.hdrMips.empty() ? nullptr : p.hdrMips[0].data());
      map.channels = &linearChannels[p.srgb];
      return map;
    };
    const Map emissive = open(m.emissionTexture.get());
    // remixapi_MaterialInfoSelfIlluminationEXT: the albedo colour (texture and
    // constant; no vertex colours or texture stages here) and the mask.
    const Map albedo = m.selfIllumination ? open(m.texture.get()) : Map{};
    auto coord = [](float value, uint32_t size, uint8_t wrap) {
      if (wrap <= 1)
        value -= std::floor(value);
      else if (wrap == 2) {
        value = std::fmod(std::abs(value), 2.f);
        if (value > 1)
          value = 2 - value;
      }
      return std::min(size - 1, uint32_t(std::clamp(value, 0.f, 1.f) * size));
    };
    // 16x16 stratified area samples, only on cache misses. Black texels count in
    // the average so small masks keep their coverage and do not gain energy.
    for (const auto &w : emissionSamples) {
      float u = 0, v = 0;
      for (unsigned i = 0; i < 3; ++i) {
        u += uv[i][0] * w[i];
        v += uv[i][1] * w[i];
      }
      if ((m.wrapU == 4 && (u < 0 || u > 1)) || (m.wrapV == 4 && (v < 0 || v > 1)))
        continue;
      auto texel = [&](const Map &map) {
        return map.at(4 * (size_t(coord(v, map.height, m.wrapV)) * map.width + coord(u, map.width, m.wrapU)));
      };
      std::array<float, 4> value{1, 1, 1, 1};
      if (emissive)
        value = texel(emissive);
      if (m.selfIllumination) {
        std::array<float, 4> color{1, 1, 1, 1};
        if (albedo)
          color = texel(albedo);
        const float mask = m.selfIllumination == 2 ? color[3] : 1;
        for (unsigned c = 0; c < 3; ++c)
          value[c] = (m.selfIllumination == 3 ? value[3] : value[c]) * mask * m.selfIlluminationScale *
                     std::clamp(color[c] * m.tint[c], 0.f, 1.f);
      }
      for (unsigned c = 0; c < 3; ++c)
        result[c] += value[c] * m.emission[c] / 256;
    }
    if (!cached)
      return result;
    // Bound caches for arbitrary animated UVs; eviction affects only CPU work.
    if (m.emissionCache.size() >= 65536)
      m.emissionCache.clear();
    m.emissionCache.emplace(key, result);
    return result;
  }
  } // namespace

  SphereLight triangleEmitter(const Draw &draw, uint32_t triangle, const std::array<float, 3> &radiance) {
    SphereLight light;
    const uint64_t identity[] = {draw.instanceId ? draw.instanceId
                                                 : uint64_t(reinterpret_cast<uintptr_t>(draw.mesh.get())),
                                 draw.instanceIndex, draw.surfaceIndex, triangle};
    light.id = hashBytes(identity, sizeof(identity));
    light.group = hashBytes(identity, 3 * sizeof(identity[0])) | 1; // never 0 (sphere lights)
    light.triangle = draw.doubleSided ? 2 : 1;
    std::copy(radiance.begin(), radiance.end(), light.radiance);
    const auto &s = *draw.surface;
    for (unsigned i = 0; i < 3; ++i) {
      auto index = s.indices[triangle * 3 + i];
      auto vertex = s.vertices[index];
      if (s.bonesPerVertex && draw.bones) {
        auto original = vertex;
        std::fill_n(vertex.position, 3, 0.f);
        for (unsigned b = 0; b < s.bonesPerVertex; ++b) {
          auto weight = index * s.bonesPerVertex + b;
          const auto &m = (*draw.bones)[s.bones[weight]];
          for (unsigned c = 0; c < 3; ++c)
            vertex.position[c] +=
                s.weights[weight] * (m[c] * original.position[0] + m[4 + c] * original.position[1] +
                                     m[8 + c] * original.position[2] + m[12 + c]);
        }
      }
      for (unsigned c = 0; c < 3; ++c) {
        const auto &m = draw.model;
        light.corners[i][c] = m[c] * vertex.position[0] + m[4 + c] * vertex.position[1] +
                              m[8 + c] * vertex.position[2] + m[12 + c];
        light.position[c] += light.corners[i][c] / 3;
      }
    }
    for (unsigned i = 0; i < 3; ++i)
      light.radius = std::max(light.radius, std::hypot(light.corners[i][0] - light.position[0],
                                                       light.corners[i][1] - light.position[1],
                                                       light.corners[i][2] - light.position[2]));
    return light;
  }
} // namespace remix::scene
