#pragma once
#include <string>
#include "scene/micromap.h"
#include "scene/types.h"
#include <map>
#include <tuple>

namespace remix::scene {
  // Coverage belongs to immutable CPU snapshots, independent of any GPU layout.
  struct OpacityPartition {
    enum Reason {
      Solid,
      OneSided,
      Blended,
      Transmission,
      AlphaTest,
      Invalid,
      MissingTexture,
      TextureBoundary,
      TextureCutout,
      // remixapi_InstanceInfoTexcoordTransformEXT offset or velocity: texels may
      // move under the triangles, so none is proven opaque (one entry, not one
      // for every offset a host gives).
      Scrolling,
      Count
    };
    std::array<uint32_t, Count> reasons{};
    // Uniform coverage needs one byte; mixed coverage stores a packed triangle
    // mask.
    std::string key;
    uint32_t triangles = 0;
    // The non-opaque triangles' micro-triangles, proved like the triangles
    // (OpacityCache::setMicromapLevel); null when none is proved.
    std::shared_ptr<const OpacityMicromap> micromap;
    bool opaque(uint32_t triangle) const {
      return key[0] == 1 || (key[0] == 2 && (uint8_t(key[1 + triangle / 8]) & (1u << (triangle % 8))));
    }
  };
  // Weak owners prevent stale results when resource addresses recur.
  class OpacityCache {
  public:
    struct Statistics {
      uint64_t lookups = 0, hits = 0, proofs = 0, triangles = 0, evictions = 0, owners = 0, textures = 0;
    };
    Statistics takeStatistics() {
      auto value = statistics;
      statistics = {};
      return value;
    }

  private:
    Statistics statistics;
    bool profile = false;
    float texelMargin;
    using Region = std::array<uint32_t, 4>;
    // Only opacity-affecting values belong in the key. RGB animation, emission,
    // normal maps and material snapshot addresses do not change coverage.
    using Key = std::tuple<std::array<uint32_t, 2>, std::array<float, 4>, uint32_t, float, float, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    struct Entry {
      std::weak_ptr<Texture> texture;
      std::shared_ptr<const OpacityPartition> partition;
      std::map<Region, bool> regions;
    };
    struct SurfaceEntries {
      std::weak_ptr<Surface> owner;
      std::map<Key, Entry> entries;
    };
    struct TextureEntries {
      std::weak_ptr<Texture> owner;
      uint32_t width = 0;
      // Summed-area counts of texels whose alpha is not exactly 255. This gives
      // the same conservative rectangle predicate as scanning the RGBA bytes.
      std::vector<uint32_t> prefix;
      // remixapi_TextureInfoDynamicEXT: pixels change in place, so no texel is
      // ever proven opaque (and none is read here).
      bool dynamic = false;
      // Mip 0's alpha (min, max) per 2^k-texel cell at level k, from 1x1 up;
      // built when a micromap first needs it (empty for compressed textures).
      std::vector<std::vector<std::array<uint8_t, 2>>> alpha;
      std::vector<uint32_t> alphaWidths;
      void buildAlpha(const Texture &texture);
      // The alpha (0-1) that sampling within r can return.
      std::array<float, 2> alphaRange(const Region &r) const;
      bool solid(const Region &r) const {
        if (dynamic)
          return false;
        if (prefix.empty())
          return true; // Compressed texture without any non-opaque texel.
        const size_t stride = size_t(width) + 1;
        const size_t x0 = r[0], y0 = r[1], x1 = size_t(r[2]) + 1, y1 = size_t(r[3]) + 1;
        return prefix[y1 * stride + x1] + prefix[y0 * stride + x0] ==
               prefix[y1 * stride + x0] + prefix[y0 * stride + x1];
      }
    };
    std::unordered_map<const Surface *, SurfaceEntries> surfaces;
    std::unordered_map<const Texture *, TextureEntries> textures;
    TextureEntries &coverage(const std::shared_ptr<Texture> &texture);
    // The cache key of a draw; false for non-finite inputs (never cached).
    bool key(const Draw &draw, const std::array<float, 4> &uv, Key &out, std::array<uint32_t, 2> &dimensions) const;
    // The texel rectangles (up to four, across a wrap) that filtered samples
    // between these texture coordinates can read, before the draw's UV transform.
    OpacityPartition::Reason footprint(const Draw &draw, const std::array<float, 4> &uv,
                                       const std::array<std::array<float, 2>, 3> &corners, std::array<Region, 4> &out,
                                       unsigned &count) const;
    OpacityPartition::Reason opaqueTexels(const Draw &draw, const std::array<float, 4> &uv, uint32_t triangle,
                                          TextureEntries *texture, std::map<Region, bool> &regions);
    std::shared_ptr<const OpacityMicromap> bakeMicromap(const Draw &draw, const std::array<float, 4> &uv,
                                                        const OpacityPartition &partition, TextureEntries *texture,
                                                        bool usesTexture);

  public:
    explicit OpacityCache(uint32_t subTexelPrecisionBits = 4, bool profile = false);
    std::shared_ptr<const OpacityPartition> partition(const Draw &draw, const std::array<float, 4> &uv);
    // Read-only: the partition partition() would return without changing the
    // cache (an owned entry validated by the same texture), otherwise null.
    // Concurrent find() calls are safe while partition() and prune() are not running.
    std::shared_ptr<const OpacityPartition> find(const Draw &draw, const std::array<float, 4> &uv) const;
    bool opaque(const Draw &draw, const std::array<float, 4> &uv) {
      auto result = partition(draw, uv);
      return result->reasons[OpacityPartition::Solid] == result->triangles;
    }
    void prune();
    // rtx.native.pathtrace.hardwareFaceCulling: when set, a one-sided surface is
    // proved like a double-sided one (Solid instead of OneSided). Traversal must
    // then reject its back faces in hardware: the tracer clears the instance's
    // facing-cull-disable flag (doubleSided belongs to the whole instance) and
    // rays carry gl_RayFlagsCullBackFacingTrianglesEXT. Part of the cache key,
    // so both variants stay cached. Set only while no lookup is running.
    void setOneSidedOpaque(bool value) { oneSidedOpaque = value; }
    bool oneSidedOpaqueEnabled() const { return oneSidedOpaque; }
    // rtx.native.pathtrace.opacityMicromaps: the deepest micromap subdivision
    // level (0: none). Alpha-tested partitions get a micromap; part of the
    // cache key like oneSidedOpaque. Set only while no lookup is running.
    void setMicromapLevel(uint32_t level) { micromapLevel = std::min(level, OpacityMicromap::maxLevel); }
    uint32_t micromapLevelEnabled() const { return micromapLevel; }

  private:
    bool oneSidedOpaque = false;
    uint32_t micromapLevel = 0;
  };
} // namespace remix::scene
