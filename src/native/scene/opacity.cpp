#include "scene/opacity.h"
#include "scene/images.h"
#include "platform/workers.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cmath>
#include <limits>

namespace remix::scene {
  namespace {
    // weak.lock() == shared, without the reference-count traffic of lock().
    template <class A, class B> bool sameOwner(const std::weak_ptr<A> &weak, const std::shared_ptr<B> &shared) {
      return shared ? !weak.owner_before(shared) && !shared.owner_before(weak) : weak.expired();
    }
    using Range = std::array<float, 2>;
    Range multiply(Range a, Range b) {
      const float values[] = {a[0] * b[0], a[0] * b[1], a[1] * b[0], a[1] * b[1]};
      return {*std::min_element(values, values + 4), *std::max_element(values, values + 4)};
    }
    // The alpha the shader's combiner and tint can produce from texture and
    // vertex alpha within these ranges, widened by its rounding.
    Range combinedAlpha(const Draw &draw, Range texture, Range vertex) {
      const auto &b = draw.blend;
      auto arg = [&](uint32_t index) -> Range {
        const float factor = (b.factor >> 24) / 255.f;
        return index == 1 ? texture : index == 2 ? vertex : index == 3 ? Range{factor, factor} : Range{1, 1};
      };
      auto alpha = b.alphaOpTexture == 1   ? arg(b.alpha1)
                   : b.alphaOpTexture == 2 ? arg(b.alpha2)
                                           : multiply(arg(b.alpha1), arg(b.alpha2));
      if (b.alphaOpTexture == 6) {
        auto a = arg(b.alpha1), c = arg(b.alpha2);
        alpha = {a[0] + c[0], a[1] + c[1]};
      } else if (b.alphaOpTexture >= 4) {
        const float scale = b.alphaOpTexture == 5 ? 4 : 2;
        alpha = multiply(alpha, {scale, scale});
      }
      for (auto &v : alpha)
        v = std::clamp(v, 0.f, 1.f);
      alpha = multiply(alpha, {draw.material->tint[3], draw.material->tint[3]});
      const float error = 1e-5f * std::max({1.f, std::abs(alpha[0]), std::abs(alpha[1])});
      return {std::clamp(alpha[0] - error, 0.f, 1.f), std::clamp(alpha[1] + error, 0.f, 1.f)};
    }
    // Every alpha in the range passes (fails) the alpha test.
    bool alwaysPasses(uint32_t compare, Range alpha, float reference) {
      return compare == 1   ? alpha[1] < reference
             : compare == 3 ? alpha[1] <= reference
             : compare == 4 ? alpha[0] > reference
                            : compare == 6 && alpha[0] >= reference;
    }
    bool alwaysFails(uint32_t compare, Range alpha, float reference) {
      return compare == 1   ? alpha[0] >= reference
             : compare == 3 ? alpha[0] > reference
             : compare == 4 ? alpha[1] <= reference
                            : compare == 6 && alpha[1] < reference;
    }
  } // namespace
  OpacityCache::OpacityCache(uint32_t subTexelPrecisionBits, bool profile)
      : profile(profile), texelMargin(std::ldexp(2.f, -int(std::min(subTexelPrecisionBits, 24u)))) {}

  const std::shared_ptr<OpacityCache::TextureEntries> &
  OpacityCache::coverage(const std::shared_ptr<Texture> &texture) {
    auto &cache = textures[texture.get()];
    if (cache && sameOwner(cache->owner, texture))
      return cache;
    if (profile)
      ++statistics.textures;
    const auto &p = texture->pixels;
    // A new object, never one a background bake may still read.
    auto next = std::make_shared<TextureEntries>();
    next->owner = texture;
    next->width = p.width;
    if (texture->dynamic) {
      next->dynamic = true;
      cache = std::move(next);
      return cache;
    }
    // Compressed textures carry a mip-0 bitmask of non-opaque texels (and
    // per-block alpha ranges for micromaps). A fully opaque one needs no
    // summed-area table (empty prefix = all solid).
    if (p.compressed && p.compressed->transparent.empty()) {
      cache = std::move(next);
      return cache;
    }
    const size_t stride = size_t(p.width) + 1;
    next->prefix.resize(stride * (size_t(p.height) + 1));
    for (uint32_t y = 0; y < p.height; ++y) {
      uint32_t row = 0;
      for (uint32_t x = 0; x < p.width; ++x) {
        row += p.compressed ? p.compressed->transparentTexel(x, y, p.width)
                            : p.mips[0][4 * (size_t(y) * p.width + x) + 3] != 255;
        next->prefix[(size_t(y) + 1) * stride + x + 1] = next->prefix[size_t(y) * stride + x + 1] + row;
      }
    }
    cache = std::move(next);
    return cache;
  }

  OpacityPartition::Reason OpacityCache::footprint(float texelMargin, const Draw &draw, const std::array<float, 4> &uv,
                                                   const std::array<std::array<float, 2>, 3> &corners,
                                                   std::array<Region, 4> &regions, unsigned &count) {
    using P = OpacityPartition;
    const auto &p = draw.material->texture->pixels;
    using Interval = std::array<uint32_t, 2>;
    std::array<Interval, 2> intervals[2];
    unsigned intervalCount[2] = {1, 1};
    for (unsigned axis = 0; axis < 2; ++axis) {
      float low = INFINITY, high = -INFINITY, magnitude = 1;
      for (const auto &corner : corners) {
        const float scaled = corner[axis] * uv[axis];
        const float value = scaled + uv[axis + 2];
        if (!std::isfinite(value))
          return P::Invalid;
        low = std::min(low, value);
        high = std::max(high, value);
        magnitude = std::max(magnitude, std::abs(scaled) + std::abs(uv[axis + 2]));
      }
      const auto size = axis ? p.height : p.width;
      const auto wrap = axis ? draw.material->wrapV : draw.material->wrapU;
      // Cover filtering, sampler rounding, interpolation and UV-transform error,
      // including cancellation between a large scale and offset.
      const float margin = (draw.material->filter ? .5f : 0.f) + texelMargin +
                           size * magnitude * 8 * std::numeric_limits<float>::epsilon();
      const float first = std::floor(low * size - margin), last = std::floor(high * size + margin);
      if (!std::isfinite(first) || !std::isfinite(last))
        return P::Invalid;
      auto &out = intervals[axis];
      if (first >= 0 && last < size)
        out[0] = {uint32_t(first), uint32_t(last)};
      else if (wrap == 3)
        return P::TextureBoundary;
      else if (wrap == 0) {
        out[0] = {uint32_t(std::clamp(first, 0.f, float(size - 1))), uint32_t(std::clamp(last, 0.f, float(size - 1)))};
      } else if (last - first >= size - 1 || std::max(std::abs(first), std::abs(last)) > 16777216.f) {
        // Long or numerically uncertain footprints include the complete axis.
        out[0] = {0, size - 1};
      } else {
        const int64_t start = int64_t(first), end = int64_t(last), n = size;
        auto modulo = [](int64_t x, int64_t n) { return (x % n + n) % n; };
        if (wrap == 2) {
          const auto lo = modulo(start, 2 * n), hi = modulo(end, 2 * n);
          const auto x = lo < n ? lo : 2 * n - 1 - lo;
          const auto y = hi < n ? hi : 2 * n - 1 - hi;
          if ((lo < n) == (hi < n))
            out[0] = {uint32_t(std::min(x, y)), uint32_t(std::max(x, y))};
          else if (lo < n)
            out[0] = {uint32_t(std::min(x, y)), size - 1};
          else
            out[0] = {0, uint32_t(std::max(x, y))};
        } else {
          const uint32_t x = uint32_t(modulo(start, n)), y = uint32_t(modulo(end, n));
          if (x <= y)
            out[0] = {x, y};
          else {
            out[0] = {x, size - 1};
            out[1] = {0, y};
            intervalCount[axis] = 2;
          }
        }
      }
    }
    count = 0;
    for (unsigned v = 0; v < intervalCount[1]; ++v)
      for (unsigned u = 0; u < intervalCount[0]; ++u)
        regions[count++] = {intervals[0][u][0], intervals[1][v][0], intervals[0][u][1], intervals[1][v][1]};
    return P::Solid;
  }

  OpacityPartition::Reason OpacityCache::opaqueTexels(const Draw &draw, const std::array<float, 4> &uv,
                                                      uint32_t triangle, TextureEntries *texture,
                                                      std::map<Region, bool> &regions) {
    using P = OpacityPartition;
    if (!texture)
      return P::MissingTexture;
    const auto &s = *draw.surface;
    std::array<std::array<float, 2>, 3> corners;
    for (unsigned corner = 0; corner < 3; ++corner) {
      const auto &vertex = s.vertices[s.indices[triangle * 3 + corner]];
      corners[corner] = {vertex.uv[0], vertex.uv[1]};
    }
    std::array<Region, 4> footprints;
    unsigned count = 0;
    if (const auto reason = footprint(texelMargin, draw, uv, corners, footprints, count); reason != P::Solid)
      return reason;
    for (unsigned i = 0; i < count; ++i) {
      const bool solid = texture->solid(footprints[i]);
      regions.emplace(footprints[i], solid);
      if (!solid)
        return P::TextureCutout;
    }
    return P::Solid;
  }

  bool OpacityCache::key(const Draw &draw, const std::array<float, 4> &uv, Key &out,
                         std::array<uint32_t, 2> &dimensions) const {
    const auto &m = *draw.material;
    const auto &b = draw.blend;
    const auto compare = draw.ignoreAlpha ? 7 : draw.alphaOverride ? draw.compare : m.compare;
    const float reference = draw.alphaOverride ? draw.reference : m.reference;
    bool finite = std::isfinite(reference) && std::isfinite(m.tint[3]);
    for (float value : uv)
      finite &= std::isfinite(value);
    // NaNs cannot be keys in an ordered map.
    if (!finite)
      return false;
    // Double-sided coverage does not depend on oneSidedOpaque: those entries
    // stay shared between both settings.
    const bool scrolling = draw.texcoordMoves();
    const uint32_t flags = uint32_t(draw.doubleSided) | uint32_t(b.enabled) << 1 | uint32_t(m.transmission) << 2 |
                           uint32_t(oneSidedOpaque && !draw.doubleSided) << 3 | uint32_t(scrolling) << 4;
    const auto &texture = m.texture;
    dimensions = {};
    if (texture) {
      const auto &p = texture->pixels;
      // Resource decoding bounds dimensions to maxTextureDimension; reject malformed CPU
      // snapshots too, keeping prefix counts and allocation arithmetic bounded.
      if (p.width && p.height && p.width <= maxTextureDimension && p.height <= maxTextureDimension &&
          (p.compressed ? p.compressed->transparent.empty() ||
                              p.compressed->transparent.size() == (size_t(p.width) * p.height + 63) / 64
                        : !p.mips.empty() && p.mips[0].size() == size_t(p.width) * p.height * 4))
        dimensions = {p.width, p.height};
    }
    // Coverage of a draw whose texcoords move does not depend on its offset (Scrolling).
    out = Key{dimensions,
              scrolling ? std::array<float, 4>{uv[0], uv[1], 0, 0} : uv,
              uint32_t(compare),
              reference,
              m.tint[3],
              b.alphaOpTexture,
              b.alpha1,
              b.alpha2,
              b.factor >> 24,
              flags,
              uint32_t(m.filter) | uint32_t(m.wrapU) << 8 | uint32_t(m.wrapV) << 16,
              draw.decal ? 0u : micromapLevel};
    return true;
  }

  std::shared_ptr<const OpacityPartition> OpacityCache::find(const Draw &draw, const std::array<float, 4> &uv) const {
    Key key;
    std::array<uint32_t, 2> dimensions;
    if (!this->key(draw, uv, key, dimensions))
      return nullptr;
    const auto cache = surfaces.find(draw.surface.get());
    if (cache == surfaces.end() || !sameOwner(cache->second.owner, draw.surface))
      return nullptr;
    const auto found = cache->second.entries.find(key);
    if (found == cache->second.entries.end() || !sameOwner(found->second.texture, draw.material->texture) ||
        abandoned(found->second))
      return nullptr;
    return found->second.partition;
  }

  std::shared_ptr<const OpacityPartition> OpacityCache::partition(const Draw &draw, const std::array<float, 4> &uv) {
    using P = OpacityPartition;
    if (profile)
      ++statistics.lookups;
    const auto &m = *draw.material;
    const auto &b = draw.blend;
    const auto compare = draw.ignoreAlpha ? 7 : draw.alphaOverride ? draw.compare : m.compare;
    const float reference = draw.alphaOverride ? draw.reference : m.reference;
    const auto &texture = m.texture;
    Key key;
    std::array<uint32_t, 2> dimensions;
    if (!this->key(draw, uv, key, dimensions)) {
      auto result = std::make_shared<P>();
      result->triangles = uint32_t(draw.surface->indices.size() / 3);
      result->key.assign(1, 0);
      result->reasons[P::Invalid] = result->triangles;
      return result;
    }
    const bool usesTexture = (b.alphaOpTexture != 2 && b.alpha1 == 1) || (b.alphaOpTexture != 1 && b.alpha2 == 1);
    TextureEntries *pixels = nullptr;
    auto getPixels = [&]() -> TextureEntries * {
      if (!pixels && dimensions[0])
        pixels = coverage(texture).get();
      return pixels;
    };
    auto &cache = surfaces[draw.surface.get()];
    if (!sameOwner(cache.owner, draw.surface)) {
      if (profile)
        ++statistics.owners;
      cache = {draw.surface, {}};
    }
    auto found = cache.entries.find(key);
    if (found != cache.entries.end()) {
      auto &entry = found->second;
      // Atlas replacements often change only RGB, or alpha in unrelated tiles.
      // Revalidate the exact rectangle predicates that produced this partition.
      // Weak ownership still prevents an address-reused snapshot from skipping
      // validation. Queued frames keep their immutable partition independently.
      // A micromap reads more of the texture than these predicates: only the
      // same texture keeps it (or the one still being baked).
      const bool sameTexture = sameOwner(entry.texture, texture);
      const bool same =
          sameTexture ||
          (!entry.partition->micromap && !entry.partition->micromapPending &&
           (entry.regions.empty() ||
            (getPixels() && std::all_of(entry.regions.begin(), entry.regions.end(), [&](const auto &region) {
               return pixels->solid(region.first) == region.second;
             }))));
      if (same) {
        if (!sameTexture)
          entry.texture = texture;
        if (abandoned(entry))
          entry.bakeEpoch = queueBake(draw, uv, key, entry.partition,
                                      usesTexture && getPixels() ? coverage(texture) : nullptr, usesTexture,
                                      contentHash(*draw.surface));
        if (profile)
          ++statistics.hits;
        return entry.partition;
      }
    }
    std::map<Region, bool> regions;
    uint64_t bakeEpoch = 0;
    if (profile)
      ++statistics.proofs;
    auto result = std::make_shared<P>();
    result->triangles = uint32_t(draw.surface->indices.size() / 3);
    // With oneSidedOpaque, facing is rejected by traversal (see opacity.h), so
    // a one-sided surface needs the same coverage proof as a double-sided one.
    P::Reason uniform = !draw.doubleSided && !oneSidedOpaque ? P::OneSided
                        : b.enabled                          ? P::Blended
                        : m.transmission                     ? P::Transmission
                                                             : P::Solid;
    if (uniform != P::Solid || compare == 7 || compare == 0 || compare == 2 || compare == 5) {
      if (uniform == P::Solid && compare != 7)
        uniform = P::AlphaTest;
      result->key.assign(1, uniform == P::Solid ? 1 : 0);
      result->reasons[uniform] = result->triangles;
    } else {
      if (profile)
        statistics.triangles += result->triangles;
      result->key.assign(1 + (result->triangles + 7) / 8, 0);
      result->key[0] = 2;
      if (usesTexture)
        getPixels();
      for (uint32_t triangle = 0; triangle < result->triangles; ++triangle) {
        Range vertex{1, 0};
        bool valid = true;
        for (unsigned corner = 0; corner < 3; ++corner) {
          float alpha = draw.surface->vertices[draw.surface->indices[triangle * 3 + corner]].color[3];
          valid &= std::isfinite(alpha);
          vertex[0] = std::min(vertex[0], alpha);
          vertex[1] = std::max(vertex[1], alpha);
        }
        auto reason = !usesTexture           ? P::Solid
                      : draw.texcoordMoves() ? P::Scrolling
                                             : opaqueTexels(draw, uv, triangle, pixels, regions);
        const Range texture = usesTexture && reason == P::Solid ? Range{1, 1} : Range{0, 1};
        const bool solid = valid && alwaysPasses(compare, combinedAlpha(draw, texture, vertex), reference);
        if (!valid)
          reason = P::Invalid;
        else if (solid)
          reason = P::Solid;
        else if (reason == P::Solid)
          reason = P::AlphaTest;
        ++result->reasons[reason];
        if (solid)
          result->key[1 + triangle / 8] |= char(1u << (triangle % 8));
      }
      if (!result->reasons[P::Solid] || result->reasons[P::Solid] == result->triangles)
        result->key.assign(1, result->reasons[P::Solid] ? 1 : 0);
      // Decals are skipped by visibility rays before their alpha test.
      if (micromapLevel && !draw.decal && result->key[0] != 1) {
        const TextureEntries *proof = usesTexture ? pixels : nullptr;
        const uint64_t hash = contentHash(*draw.surface);
        auto *shared = content(hash, texture, key, *result);
        if (shared && shared->ready)
          result->micromap = shared->micromap;
        else if (baker) {
          result->micromapPending = true;
          if (shared) {
            // Another copy's bake is in flight: wait for it.
            shared->waiters.push_back({draw.surface, draw.surface.get(), key, result});
            bakeEpoch = shared->epoch;
          } else
            bakeEpoch = queueBake(draw, uv, key, result, proof ? coverage(texture) : nullptr, usesTexture, hash);
        } else {
          result->micromap = bakeMicromap(texelMargin, micromapLevel, draw, uv, *result, proof, usesTexture);
          contents.insert_or_assign(
              hash, ContentEntry{texture, texture != nullptr, key, result->key, result->micromap, true, nullptr, 0, {}});
        }
      }
    }
    if (cache.entries.size() >= 8) {
      if (profile)
        ++statistics.evictions;
      cache.entries.clear();
    }
    cache.entries.insert_or_assign(key, Entry{texture, result, std::move(regions), bakeEpoch});
    return result;
  }

  void OpacityCache::TextureEntries::buildAlpha(const Texture &texture) const {
    std::call_once(alphaBuilt, [&] { buildAlphaOnce(texture); });
  }

  void OpacityCache::TextureEntries::buildAlphaOnce(const Texture &texture) const {
    const auto &p = texture.pixels;
    if (dynamic)
      return;
    uint32_t w = p.width, h = p.height;
    std::vector<std::array<uint8_t, 2>> level;
    if (p.compressed) {
      w = (w + 3) / 4;
      h = (h + 3) / 4;
      // Without block ranges (no non-opaque texel, or a copy that dropped
      // them) only solid() proves anything.
      if (p.compressed->alphaBlocks.size() != size_t(w) * h)
        return;
      level = p.compressed->alphaBlocks;
      alphaShift = 2;
    } else {
      if (p.mips.empty())
        return;
      level.resize(size_t(w) * h);
      for (size_t i = 0; i < level.size(); ++i)
        level[i] = {p.mips[0][4 * i + 3], p.mips[0][4 * i + 3]};
    }
    alpha.push_back(std::move(level));
    alphaWidths.push_back(w);
    while (w > 1 || h > 1) {
      const uint32_t nw = (w + 1) / 2, nh = (h + 1) / 2;
      const auto &below = alpha.back();
      std::vector<std::array<uint8_t, 2>> next(size_t(nw) * nh, {255, 0});
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          auto &cell = next[size_t(y / 2) * nw + x / 2];
          const auto &value = below[size_t(y) * w + x];
          cell = {std::min(cell[0], value[0]), std::max(cell[1], value[1])};
        }
      alpha.push_back(std::move(next));
      alphaWidths.push_back(nw);
      w = nw;
      h = nh;
    }
  }

  std::array<float, 2> OpacityCache::TextureEntries::alphaRange(const Region &r) const {
    if (dynamic)
      return {0, 1};
    if (solid(r))
      return {1, 1};
    if (alpha.empty())
      return {0, 1}; // No alpha ranges: only which texels are not opaque is known.
    // The coarsest cells r spans at most two of per axis (at least the base
    // level's): a superset of r.
    size_t k = 0;
    auto shift = [&] { return uint32_t(alphaShift + k); };
    while (k + 1 < alpha.size() &&
           ((r[2] >> shift()) - (r[0] >> shift()) > 1 || (r[3] >> shift()) - (r[1] >> shift()) > 1))
      ++k;
    uint8_t low = 255, high = 0;
    for (uint32_t y = r[1] >> shift(); y <= r[3] >> shift(); ++y)
      for (uint32_t x = r[0] >> shift(); x <= r[2] >> shift(); ++x) {
        const auto &cell = alpha[k][size_t(y) * alphaWidths[k] + x];
        low = std::min(low, cell[0]);
        high = std::max(high, cell[1]);
      }
    return {low / 255.f, high / 255.f};
  }

  std::shared_ptr<const OpacityMicromap> OpacityCache::bakeMicromap(float texelMargin, uint32_t micromapLevel,
                                                                    const Draw &draw, const std::array<float, 4> &uv,
                                                                    const OpacityPartition &partition,
                                                                    const TextureEntries *texture, bool usesTexture) {
    using M = OpacityMicromap;
    const auto &m = *draw.material;
    const auto compare = draw.alphaOverride ? draw.compare : m.compare;
    const float reference = draw.alphaOverride ? draw.reference : m.reference;
    const auto &s = *draw.surface;
    // Texels under moving texture coordinates are unknown, as for the triangles.
    if (texture && !draw.texcoordMoves())
      texture->buildAlpha(*m.texture);
    else
      texture = nullptr;
    const float texels =
        texture ? float(m.texture->pixels.width) * float(m.texture->pixels.height) * std::abs(uv[0] * uv[1]) : 0.f;
    auto micromap = std::make_shared<M>();
    micromap->indices.assign(partition.triangles, M::FullyOpaque);
    // Identical micro-triangle arrays share one stored triangle.
    std::map<std::pair<uint32_t, std::string>, int32_t> stored;
    std::vector<uint8_t> states;
    std::string packed;
    bool proved = false;
    for (uint32_t triangle = 0; triangle < partition.triangles; ++triangle) {
      if (partition.opaque(triangle))
        continue;
      std::array<std::array<float, 2>, 3> uvs;
      std::array<float, 3> alphas;
      bool valid = true;
      for (unsigned corner = 0; corner < 3; ++corner) {
        const auto &vertex = s.vertices[s.indices[triangle * 3 + corner]];
        uvs[corner] = {vertex.uv[0], vertex.uv[1]};
        alphas[corner] = vertex.color[3];
        valid &= std::isfinite(alphas[corner]) && std::isfinite(uvs[corner][0]) && std::isfinite(uvs[corner][1]);
      }
      if (!valid) {
        micromap->indices[triangle] = M::FullyUnknownOpaque;
        continue;
      }
      // About two micro-triangles per texel, up to the level setting: finer
      // than the texels, as each footprint adds half a texel of filtering.
      const float area = .5f *
                         std::abs((uvs[1][0] - uvs[0][0]) * (uvs[2][1] - uvs[0][1]) -
                                  (uvs[2][0] - uvs[0][0]) * (uvs[1][1] - uvs[0][1])) *
                         texels;
      uint32_t level = 1;
      while (level < micromapLevel && float(1u << (2 * level)) < area * 2)
        ++level;
      states.assign(size_t(1) << (2 * level), M::UnknownOpaque);
      // A proved micro-triangle's children (4k..4k+3 a level down) lie inside
      // its footprint: they are not proved again.
      auto prove = [&](auto &self, uint32_t at, uint32_t index) -> void {
        const auto corners = microCorners(microTriangles(at)[index], at);
        std::array<std::array<float, 2>, 3> points;
        Range vertex{1, 0};
        for (unsigned c = 0; c < 3; ++c) {
          const float b1 = corners[c][0], b2 = corners[c][1], b0 = 1 - b1 - b2;
          for (unsigned axis = 0; axis < 2; ++axis)
            points[c][axis] = uvs[0][axis] * b0 + uvs[1][axis] * b1 + uvs[2][axis] * b2;
          const float a = alphas[0] * b0 + alphas[1] * b1 + alphas[2] * b2;
          vertex = {std::min(vertex[0], a), std::max(vertex[1], a)};
        }
        Range sampled{0, 1};
        if (texture) {
          std::array<Region, 4> regions;
          unsigned count = 0;
          if (footprint(texelMargin, draw, uv, points, regions, count) == OpacityPartition::Solid) {
            sampled = {1, 0};
            for (unsigned i = 0; i < count; ++i) {
              const auto range = texture->alphaRange(regions[i]);
              sampled = {std::min(sampled[0], range[0]), std::max(sampled[1], range[1])};
            }
          }
        } else if (!usesTexture)
          sampled = {1, 1}; // Not read by the combiner.
        const auto alpha = combinedAlpha(draw, sampled, vertex);
        const uint8_t state = alwaysPasses(compare, alpha, reference)  ? M::Opaque
                              : alwaysFails(compare, alpha, reference) ? M::Transparent
                                                                       : M::UnknownOpaque;
        if (state != M::UnknownOpaque || at == level) {
          const size_t span = size_t(1) << (2 * (level - at));
          std::fill_n(states.begin() + index * span, span, state);
          return;
        }
        for (uint32_t child = 0; child < 4; ++child)
          self(self, at + 1, index * 4 + child);
      };
      prove(prove, 0, 0);
      uint64_t counts[4] = {};
      for (uint8_t state : states)
        ++counts[state];
      micromap->opaque += counts[M::Opaque];
      micromap->transparent += counts[M::Transparent];
      micromap->unknown += counts[M::UnknownOpaque];
      proved |= counts[M::UnknownOpaque] != states.size();
      if (counts[M::Opaque] == states.size()) {
        micromap->indices[triangle] = M::FullyOpaque;
        continue;
      }
      if (counts[M::Transparent] == states.size()) {
        micromap->indices[triangle] = M::FullyTransparent;
        continue;
      }
      if (counts[M::UnknownOpaque] == states.size()) {
        micromap->indices[triangle] = M::FullyUnknownOpaque;
        continue;
      }
      packed.assign(states.size() / 4, 0);
      for (size_t i = 0; i < states.size(); ++i)
        packed[i / 4] = char(uint8_t(packed[i / 4]) | states[i] << (2 * (i % 4)));
      auto [found, inserted] = stored.try_emplace({level, packed}, int32_t(micromap->triangles.size()));
      if (inserted) {
        micromap->triangles.push_back({uint32_t(micromap->data.size()), uint16_t(level), M::Format4State});
        micromap->data.insert(micromap->data.end(), packed.begin(), packed.end());
        ++micromap->levels[level];
      }
      micromap->indices[triangle] = found->second;
    }
    // Nothing proved: traversal would run every alpha test anyway.
    if (!proved)
      return nullptr;
    uint64_t hash = fastHash(micromap->indices.data(), micromap->indices.size() * sizeof(int32_t));
    hash = fastHash(micromap->data.data(), micromap->data.size(), hash);
    micromap->hash = fastHash(micromap->triangles.data(), micromap->triangles.size() * sizeof(M::Triangle), hash);
    return micromap;
  }

  // Background micromap baking (setAsyncMicromaps). Jobs hold everything
  // they read (the draw's snapshots, the partition, the texture's coverage),
  // never the cache; finished bakes wait in `done` for collectMicromaps().
  struct OpacityCache::Baker {
    struct Job {
      Draw draw;
      std::array<float, 4> uv;
      Key key;
      std::shared_ptr<const OpacityPartition> partition;
      std::shared_ptr<const TextureEntries> texture;
      bool usesTexture;
      float texelMargin;
      uint32_t level;
      uint64_t epoch;
      uint64_t content;
      [[maybe_unused]] unsigned csr; // the queuing thread's x86 MXCSR
    };
    struct Done {
      Waiter waiter; // the bake's own partition
      uint64_t content;
      std::shared_ptr<const OpacityMicromap> micromap;
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::vector<Done> done;
    // Bumped by setMicromapLevel: queued jobs of an older epoch are dropped.
    std::atomic<uint64_t> epoch{0};
    bool stop = false;
    std::vector<std::thread> threads;

    explicit Baker(unsigned count) {
      for (unsigned i = 0; i < count; ++i)
        threads.emplace_back([this] { work(); });
    }
    ~Baker() {
      {
        std::lock_guard lock(mutex);
        stop = true;
        jobs.clear();
      }
      wake.notify_all();
      for (auto &thread : threads)
        thread.join();
    }
    void work() {
      for (;;) {
        Job job;
        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [&] { return stop || !jobs.empty(); });
          if (stop)
            return;
          job = std::move(jobs.front());
          jobs.pop_front();
        }
        if (job.epoch != epoch.load(std::memory_order_relaxed))
          continue;
#if defined(REMIX_WORKERS_MXCSR)
        // The same floating-point modes as a synchronous bake.
        _mm_setcsr(job.csr);
#endif
        std::shared_ptr<const OpacityMicromap> micromap;
        try {
          micromap = bakeMicromap(job.texelMargin, job.level, job.draw, job.uv, *job.partition, job.texture.get(),
                                  job.usesTexture);
        } catch (...) {
          // Out of memory: the partition keeps the shader's alpha test.
        }
        std::lock_guard lock(mutex);
        done.push_back({{job.draw.surface, job.draw.surface.get(), job.key, std::move(job.partition)}, job.content,
                        std::move(micromap)});
      }
    }
  };

  uint64_t OpacityCache::queueBake(const Draw &draw, const std::array<float, 4> &uv, const Key &key,
                                   std::shared_ptr<const OpacityPartition> partition,
                                   std::shared_ptr<const TextureEntries> texture, bool usesTexture,
                                   uint64_t content) {
    const uint64_t epoch = baker->epoch.load(std::memory_order_relaxed);
    ++micromapBakes;
    const auto &owner = draw.material->texture;
    contents.insert_or_assign(
        content, ContentEntry{owner, owner != nullptr, key, partition->key, nullptr, false, partition, epoch, {}});
    Baker::Job job{draw, uv, key, std::move(partition), std::move(texture), usesTexture, texelMargin, micromapLevel,
                   epoch, content, 0};
#if defined(REMIX_WORKERS_MXCSR)
    job.csr = _mm_getcsr();
#endif
    {
      std::lock_guard lock(baker->mutex);
      baker->jobs.push_back(std::move(job));
    }
    baker->wake.notify_one();
    return epoch;
  }

  bool OpacityCache::abandoned(const Entry &entry) const {
    return baker && entry.partition->micromapPending &&
           entry.bakeEpoch != baker->epoch.load(std::memory_order_relaxed);
  }

  void OpacityCache::setMicromapLevel(uint32_t level) {
    level = std::min(level, OpacityMicromap::maxLevel);
    if (level == micromapLevel)
      return;
    micromapLevel = level;
    if (baker)
      baker->epoch.fetch_add(1, std::memory_order_relaxed);
  }

  void OpacityCache::setAsyncMicromaps(bool value) {
    if (value == (baker != nullptr))
      return;
    // Two threads: a large set bakes in parallel with the frame's own
    // workers without taking all of the CPU.
    baker = value ? std::make_shared<Baker>(2u) : nullptr;
  }

  size_t OpacityCache::collectMicromaps() {
    if (!baker)
      return 0;
    std::vector<Baker::Done> finished;
    {
      std::lock_guard lock(baker->mutex);
      finished.swap(baker->done);
    }
    size_t installed = 0;
    for (auto &item : finished) {
      std::vector<Waiter> targets{std::move(item.waiter)};
      // Copies of the same content that waited for this bake, and later
      // copies, take its micromap too.
      if (const auto found = contents.find(item.content);
          found != contents.end() && found->second.baking == targets[0].partition) {
        auto &shared = found->second;
        shared.micromap = item.micromap;
        shared.ready = true;
        shared.baking = nullptr;
        targets.insert(targets.end(), std::make_move_iterator(shared.waiters.begin()),
                       std::make_move_iterator(shared.waiters.end()));
        shared.waiters.clear();
      }
      for (const auto &target : targets)
        installed += install(target, item.micromap);
    }
    micromapInstalls += installed;
    return installed;
  }

  bool OpacityCache::install(const Waiter &waiter, const std::shared_ptr<const OpacityMicromap> &micromap) {
    // Only into the entry still holding the partition proved for it: an
    // evicted, re-proved or re-keyed entry drops it.
    const auto cache = surfaces.find(waiter.surface);
    if (cache == surfaces.end() || waiter.owner.owner_before(cache->second.owner) ||
        cache->second.owner.owner_before(waiter.owner))
      return false;
    const auto found = cache->second.entries.find(waiter.key);
    if (found == cache->second.entries.end() || found->second.partition != waiter.partition)
      return false;
    auto replacement = std::make_shared<OpacityPartition>(*waiter.partition);
    replacement->micromap = micromap;
    replacement->micromapPending = false;
    found->second.partition = std::move(replacement);
    return true;
  }

  uint64_t OpacityCache::contentHash(const Surface &surface) {
    contentScratch.resize(surface.vertices.size() * 3);
    for (size_t i = 0; i < surface.vertices.size(); ++i) {
      const auto &vertex = surface.vertices[i];
      contentScratch[3 * i] = vertex.uv[0];
      contentScratch[3 * i + 1] = vertex.uv[1];
      contentScratch[3 * i + 2] = vertex.color[3];
    }
    return fastHash(contentScratch.data(), contentScratch.size() * sizeof(float),
                    fastHash(surface.indices.data(), surface.indices.size() * sizeof(uint32_t)));
  }

  OpacityCache::ContentEntry *OpacityCache::content(uint64_t hash, const std::shared_ptr<Texture> &texture,
                                                    const Key &key, const OpacityPartition &partition) {
    const auto found = contents.find(hash);
    if (found == contents.end())
      return nullptr;
    auto &entry = found->second;
    const bool sameTexture = entry.textured == (texture != nullptr) && (!texture || sameOwner(entry.texture, texture));
    if (!sameTexture || entry.key != key || entry.partition != partition.key)
      return nullptr;
    if (!entry.ready && (!baker || entry.epoch != baker->epoch.load(std::memory_order_relaxed)))
      return nullptr; // A bake a level change dropped.
    return &entry;
  }

  void OpacityCache::prune() {
    std::erase_if(surfaces, [](const auto &item) { return item.second.owner.expired(); });
    std::erase_if(textures, [](const auto &item) { return !item.second || item.second->owner.expired(); });
    std::erase_if(contents, [](const auto &item) { return item.second.textured && item.second.texture.expired(); });
    // Bounded: finished entries go first (a later copy bakes again).
    if (contents.size() > 16384)
      std::erase_if(contents, [](const auto &item) { return item.second.ready; });
  }
} // namespace remix::scene
