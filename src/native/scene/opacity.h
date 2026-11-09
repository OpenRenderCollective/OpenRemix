#pragma once
#include <string>
#include "scene/micromap.h"
#include "scene/types.h"
#include <map>
#include <memory>
#include <mutex>
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
    // Its micromap is still being baked (OpacityCache::setAsyncMicromaps):
    // once collected, find() returns a replacement carrying it.
    bool micromapPending = false;
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
      uint64_t bakeEpoch = 0; // Baker epoch its pending micromap was queued in
    };
    struct SurfaceEntries {
      std::weak_ptr<Surface> owner;
      std::map<Key, Entry> entries;
    };
    // Micromaps by surface content: a hash of the indices, texture
    // coordinates and vertex alpha (all a bake reads of a surface), with the
    // cache key and the triangle partition. A mesh uploaded again with new
    // positions only (swaying foliage, every frame) reuses the micromap baked
    // for an earlier copy instead of baking again, which in the background
    // would never catch up with the next copy.
    struct Waiter {
      std::weak_ptr<Surface> owner;
      const Surface *surface;
      Key key;
      std::shared_ptr<const OpacityPartition> partition;
    };
    struct ContentEntry {
      std::weak_ptr<Texture> texture;
      bool textured = false;
      Key key;
      std::string partition; // OpacityPartition::key
      std::shared_ptr<const OpacityMicromap> micromap;
      bool ready = false;
      // Background baking: the bake in flight (its partition and epoch) and
      // the pending partitions of other copies waiting for it.
      std::shared_ptr<const OpacityPartition> baking;
      uint64_t epoch = 0;
      std::vector<Waiter> waiters;
    };
    std::unordered_map<uint64_t, ContentEntry> contents;
    std::vector<float> contentScratch;
    uint64_t contentHash(const Surface &surface);
    // The content entry a proof can use (same texture, key and partition; a
    // bake in flight only from the current epoch), or null.
    ContentEntry *content(uint64_t hash, const std::shared_ptr<Texture> &texture, const Key &key,
                          const OpacityPartition &partition);
    // Gives the cache entry a waiter was proved for its micromap, if the
    // entry still holds that pending partition.
    bool install(const Waiter &waiter, const std::shared_ptr<const OpacityMicromap> &micromap);
    // Immutable once built, apart from the alpha pyramid built at most once:
    // background bakes keep their own reference.
    struct TextureEntries {
      std::weak_ptr<Texture> owner;
      uint32_t width = 0;
      // Summed-area counts of texels whose alpha is not exactly 255. This gives
      // the same conservative rectangle predicate as scanning the RGBA bytes.
      std::vector<uint32_t> prefix;
      // remixapi_TextureInfoDynamicEXT: pixels change in place, so no texel is
      // ever proven opaque (and none is read here).
      bool dynamic = false;
      // Mip 0's alpha (min, max) per 2^(alphaShift + k)-texel cell at level k;
      // built when a micromap first needs it. Decoded textures start from 1x1
      // texels; compressed ones from their 4x4 blocks (alphaShift 2).
      mutable std::vector<std::vector<std::array<uint8_t, 2>>> alpha;
      mutable std::vector<uint32_t> alphaWidths;
      mutable uint32_t alphaShift = 0;
      mutable std::once_flag alphaBuilt;
      // Thread-safe; the first call builds the pyramid.
      void buildAlpha(const Texture &texture) const;
      void buildAlphaOnce(const Texture &texture) const;
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
    std::unordered_map<const Texture *, std::shared_ptr<TextureEntries>> textures;
    const std::shared_ptr<TextureEntries> &coverage(const std::shared_ptr<Texture> &texture);
    // The cache key of a draw; false for non-finite inputs (never cached).
    bool key(const Draw &draw, const std::array<float, 4> &uv, Key &out, std::array<uint32_t, 2> &dimensions) const;
    // The texel rectangles (up to four, across a wrap) that filtered samples
    // between these texture coordinates can read, before the draw's UV transform.
    // texelMargin: the cache's sub-texel rounding margin.
    static OpacityPartition::Reason footprint(float texelMargin, const Draw &draw, const std::array<float, 4> &uv,
                                              const std::array<std::array<float, 2>, 3> &corners,
                                              std::array<Region, 4> &out, unsigned &count);
    OpacityPartition::Reason opaqueTexels(const Draw &draw, const std::array<float, 4> &uv, uint32_t triangle,
                                          TextureEntries *texture, std::map<Region, bool> &regions);
    // Reads only its arguments, so background threads can run it.
    static std::shared_ptr<const OpacityMicromap> bakeMicromap(float texelMargin, uint32_t micromapLevel,
                                                               const Draw &draw, const std::array<float, 4> &uv,
                                                               const OpacityPartition &partition,
                                                               const TextureEntries *texture, bool usesTexture);
    struct Baker;
    std::shared_ptr<Baker> baker; // setAsyncMicromaps
    // Returns the epoch the bake was queued in (Entry::bakeEpoch).
    // Also registers the bake as the content entry's (content: contentHash).
    uint64_t queueBake(const Draw &draw, const std::array<float, 4> &uv, const Key &key,
                       std::shared_ptr<const OpacityPartition> partition,
                       std::shared_ptr<const TextureEntries> texture, bool usesTexture, uint64_t content);
    // A pending partition whose bake a level change dropped: lookups treat it
    // as a miss, so partition() queues the bake again.
    bool abandoned(const Entry &entry) const;
    uint64_t micromapInstalls = 0, micromapBakes = 0;

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
    // A change abandons the bakes still queued for the previous level.
    void setMicromapLevel(uint32_t level);
    uint32_t micromapLevelEnabled() const { return micromapLevel; }
    // Bake micromaps on background threads (the tracer): partition() then
    // returns an alpha-tested partition at once, without its micromap and
    // marked micromapPending, and collectMicromaps() installs the finished
    // ones. Synchronous by default (results within the same call). Set only
    // while no lookup is running.
    void setAsyncMicromaps(bool value);
    bool asyncMicromapsEnabled() const { return baker != nullptr; }
    // Replaces each finished bake's pending partition in the cache with one
    // carrying the micromap (or none, if nothing was proved): later find() and
    // partition() calls return it. Returns how many were installed. Call while
    // no lookup is running.
    size_t collectMicromaps();
    // Counts collectMicromaps() installs, so holders of pending partitions
    // (the persistent static set) know when to look them up again.
    uint64_t micromapInstallCount() const { return micromapInstalls; }
    // Bakes queued so far (diagnostics).
    uint64_t micromapBakeCount() const { return micromapBakes; }

  private:
    bool oneSidedOpaque = false;
    uint32_t micromapLevel = 0;
  };
} // namespace remix::scene
