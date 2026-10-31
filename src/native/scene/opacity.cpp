#include "scene/opacity.h"
#include "scene/images.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace remix::scene {
  namespace {
    // weak.lock() == shared, without the reference-count traffic of lock().
    template <class A, class B> bool sameOwner(const std::weak_ptr<A> &weak, const std::shared_ptr<B> &shared) {
      return shared ? !weak.owner_before(shared) && !shared.owner_before(weak) : weak.expired();
    }
  } // namespace
  OpacityCache::OpacityCache(uint32_t subTexelPrecisionBits, bool profile)
      : profile(profile), texelMargin(std::ldexp(2.f, -int(std::min(subTexelPrecisionBits, 24u)))) {}

  OpacityCache::TextureEntries &OpacityCache::coverage(const std::shared_ptr<Texture> &texture) {
    auto &cache = textures[texture.get()];
    if (sameOwner(cache.owner, texture))
      return cache;
    if (profile)
      ++statistics.textures;
    const auto &p = texture->pixels;
    TextureEntries next{texture, p.width, {}};
    if (texture->dynamic) {
      next.dynamic = true;
      cache = std::move(next);
      return cache;
    }
    // Compressed textures carry only a mip-0 bitmask of non-opaque texels. A
    // fully opaque one needs no summed-area table (empty prefix = all solid).
    if (p.compressed && p.compressed->transparent.empty()) {
      cache = std::move(next);
      return cache;
    }
    const size_t stride = size_t(p.width) + 1;
    next.prefix.resize(stride * (size_t(p.height) + 1));
    for (uint32_t y = 0; y < p.height; ++y) {
      uint32_t row = 0;
      for (uint32_t x = 0; x < p.width; ++x) {
        row += p.compressed ? p.compressed->transparentTexel(x, y, p.width)
                            : p.mips[0][4 * (size_t(y) * p.width + x) + 3] != 255;
        next.prefix[(size_t(y) + 1) * stride + x + 1] = next.prefix[size_t(y) * stride + x + 1] + row;
      }
    }
    cache = std::move(next);
    return cache;
  }

  OpacityPartition::Reason OpacityCache::opaqueTexels(const Draw &draw, const std::array<float, 4> &uv,
                                                      uint32_t triangle, TextureEntries *texture,
                                                      std::map<Region, bool> &regions) {
    using P = OpacityPartition;
    if (!texture)
      return P::MissingTexture;
    const auto &p = draw.material->texture->pixels;
    const auto &s = *draw.surface;
    using Interval = std::array<uint32_t, 2>;
    std::array<Interval, 2> intervals[2];
    unsigned intervalCount[2] = {1, 1};
    for (unsigned axis = 0; axis < 2; ++axis) {
      float low = INFINITY, high = -INFINITY, magnitude = 1;
      for (unsigned corner = 0; corner < 3; ++corner) {
        const float scaled = s.vertices[s.indices[triangle * 3 + corner]].uv[axis] * uv[axis];
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
        out[0] = {uint32_t(std::clamp(first, 0.f, float(size - 1))),
                  uint32_t(std::clamp(last, 0.f, float(size - 1)))};
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
    for (unsigned v = 0; v < intervalCount[1]; ++v)
      for (unsigned u = 0; u < intervalCount[0]; ++u) {
        const std::array<uint32_t, 4> region{intervals[0][u][0], intervals[1][v][0], intervals[0][u][1],
                                             intervals[1][v][1]};
        const bool solid = texture->solid(region);
        regions.emplace(region, solid);
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
    const uint32_t flags = uint32_t(draw.doubleSided) | uint32_t(b.enabled) << 1 | uint32_t(m.transmission) << 2 |
                           uint32_t(oneSidedOpaque && !draw.doubleSided) << 3;
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
    out = Key{dimensions,
              uv,
              uint32_t(compare),
              reference,
              m.tint[3],
              b.alphaOpTexture,
              b.alpha1,
              b.alpha2,
              b.factor >> 24,
              flags,
              uint32_t(m.filter) | uint32_t(m.wrapU) << 8 | uint32_t(m.wrapV) << 16};
    return true;
  }

  std::shared_ptr<const OpacityPartition> OpacityCache::find(const Draw &draw,
                                                             const std::array<float, 4> &uv) const {
    Key key;
    std::array<uint32_t, 2> dimensions;
    if (!this->key(draw, uv, key, dimensions))
      return nullptr;
    const auto cache = surfaces.find(draw.surface.get());
    if (cache == surfaces.end() || !sameOwner(cache->second.owner, draw.surface))
      return nullptr;
    const auto found = cache->second.entries.find(key);
    if (found == cache->second.entries.end() || !sameOwner(found->second.texture, draw.material->texture))
      return nullptr;
    return found->second.partition;
  }

  std::shared_ptr<const OpacityPartition> OpacityCache::partition(const Draw &draw,
                                                                  const std::array<float, 4> &uv) {
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
    TextureEntries *pixels = nullptr;
    auto getPixels = [&]() -> TextureEntries * {
      if (!pixels && dimensions[0])
        pixels = &coverage(texture);
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
      const bool sameTexture = sameOwner(entry.texture, texture);
      const bool same =
          entry.regions.empty() || sameTexture ||
          (getPixels() && std::all_of(entry.regions.begin(), entry.regions.end(), [&](const auto &region) {
             return pixels->solid(region.first) == region.second;
           }));
      if (same) {
        if (!sameTexture)
          entry.texture = texture;
        if (profile)
          ++statistics.hits;
        return entry.partition;
      }
    }
    std::map<Region, bool> regions;
    if (profile)
      ++statistics.proofs;
    auto result = std::make_shared<P>();
    result->triangles = uint32_t(draw.surface->indices.size() / 3);
    // With oneSidedOpaque, facing is rejected by traversal (see opacity.h), so
    // a one-sided surface needs the same coverage proof as a double-sided one.
    P::Reason uniform = !draw.doubleSided && !oneSidedOpaque ? P::OneSided
                        : b.enabled       ? P::Blended
                        : m.transmission  ? P::Transmission
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
      using Range = std::array<float, 2>;
      auto multiply = [](Range a, Range b) -> Range {
        const float values[] = {a[0] * b[0], a[0] * b[1], a[1] * b[0], a[1] * b[1]};
        return {*std::min_element(values, values + 4), *std::max_element(values, values + 4)};
      };
      const bool usesTexture =
          (b.alphaOpTexture != 2 && b.alpha1 == 1) || (b.alphaOpTexture != 1 && b.alpha2 == 1);
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
        auto reason = usesTexture ? opaqueTexels(draw, uv, triangle, pixels, regions) : P::Solid;
        const Range texture = usesTexture && reason == P::Solid ? Range{1, 1} : Range{0, 1};
        auto arg = [&](uint32_t index) -> Range {
          const float factor = (b.factor >> 24) / 255.f;
          return index == 1   ? texture
                 : index == 2 ? vertex
                 : index == 3 ? Range{factor, factor}
                              : Range{1, 1};
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
        alpha = multiply(alpha, {m.tint[3], m.tint[3]});
        const float error = 1e-5f * std::max({1.f, std::abs(alpha[0]), std::abs(alpha[1])});
        alpha[0] = std::clamp(alpha[0] - error, 0.f, 1.f);
        alpha[1] = std::clamp(alpha[1] + error, 0.f, 1.f);
        const bool solid = valid && (compare == 1   ? alpha[1] < reference
                                     : compare == 3 ? alpha[1] <= reference
                                     : compare == 4 ? alpha[0] > reference
                                                    : compare == 6 && alpha[0] >= reference);
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
    }
    if (cache.entries.size() >= 8) {
      if (profile)
        ++statistics.evictions;
      cache.entries.clear();
    }
    cache.entries.insert_or_assign(key, Entry{texture, result, std::move(regions)});
    return result;
  }

  void OpacityCache::prune() {
    std::erase_if(surfaces, [](const auto &item) { return item.second.owner.expired(); });
    std::erase_if(textures, [](const auto &item) { return item.second.owner.expired(); });
  }
} // namespace remix::scene
