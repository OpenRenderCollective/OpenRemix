#include "scene/light_bvh.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <tuple>

namespace remix::scene {
  namespace {
    double powerScale(const SphereLight &light) {
      if (!light.triangle)
        return double(light.radius) * light.radius;
      double a[3], b[3];
      for (unsigned c = 0; c < 3; ++c) {
        a[c] = double(light.corners[1][c]) - light.corners[0][c];
        b[c] = double(light.corners[2][c]) - light.corners[0][c];
      }
      // (sqrt, not hypot: this runs for every emissive triangle every update.)
      const double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
      const double area = .5 * std::sqrt(x * x + y * y + z * z);
      return area * light.triangle / (4 * 3.141592653589793);
    }
    // Common 4*pi*pi cancels in every selection probability.
    double lightPower(const SphereLight &light) {
      return powerScale(light) * (.2126 * light.radiance[0] + .7152 * light.radiance[1] + .0722 * light.radiance[2]);
    }
    // A run of leaves' emitter positions and count, for choosing splits.
    struct Extent {
      float minimum[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max()};
      float maximum[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                          std::numeric_limits<float>::lowest()};
      double count = 0;
      void add(const Extent &other) {
        for (unsigned c = 0; c < 3; ++c) {
          minimum[c] = std::min(minimum[c], other.minimum[c]);
          maximum[c] = std::max(maximum[c], other.maximum[c]);
        }
        count += other.count;
      }
      // Half the box's perimeter, not its area: points on a plane or a line
      // (a row of lamps) still have a size.
      double size() const {
        double d = 0;
        for (unsigned c = 0; c < 3; ++c)
          d += std::max(0., double(maximum[c]) - minimum[c]);
        return d;
      }
    };
    LightEmitter emitterOf(const SphereLight &light) {
      LightEmitter e;
      std::copy_n(light.position, 3, e.positionRadius);
      e.positionRadius[3] = light.radius;
      std::copy_n(light.radiance, 3, e.radiance);
      e.radiance[3] = float(light.triangle);
      std::copy_n(light.direction, 3, e.directionCone);
      e.directionCone[3] = light.coneCos;
      e.shaping[0] = light.softness;
      e.shaping[1] = light.focus;
      e.shaping[2] = float(light.ies);
      for (unsigned v = 0; v < 3; ++v)
        std::copy_n(light.corners[v], 3, e.corners[v]);
      // A sphere's corners are free: its cookie's projection.
      if (light.cookie && !light.triangle) {
        e.shaping[3] = float(light.cookie);
        std::copy_n(light.cookieUp, 3, e.corners[0]);
        e.corners[0][3] = light.cookieTan;
      }
      return e;
    }
  } // namespace
  namespace {
    // Emitter order: type, surface draw, spatial rank, identity.
    using Key = std::tuple<uint32_t, uint64_t, uint32_t, uint64_t>;
    Key keyOf(const SphereLight &light) { return {light.triangle, light.group, light.rank, light.id}; }
    bool sameGeometry(const SphereLight &a, const SphereLight &b) {
      bool same = a.coneCos == b.coneCos && a.softness == b.softness && a.focus == b.focus && a.ies == b.ies &&
                  a.cookie == b.cookie && a.cookieTan == b.cookieTan &&
                  std::equal(a.cookieUp, a.cookieUp + 3, b.cookieUp) &&
                  std::equal(a.direction, a.direction + 3, b.direction) && a.radius == b.radius &&
                  std::equal(a.position, a.position + 3, b.position);
      for (unsigned v = 0; v < 3; ++v)
        same &= std::equal(a.corners[v], a.corners[v] + 3, b.corners[v]);
      return same;
    }
    bool lit(const SphereLight &light) {
      return light.radiance[0] > 0 || light.radiance[1] > 0 || light.radiance[2] > 0;
    }
  } // namespace

  // The update a build is due in: once the lights waiting in the dynamic part
  // (not those of dynamic instances) have come to rest, or the static part is
  // mostly slots of lights gone for kRetain updates; never sooner than
  // kBuildInterval after the last one.
  uint64_t LightBvh::due(uint64_t oldest, uint64_t newest, uint32_t stale) const {
    if (oldest != ~0ull)
      return std::max(m_builtAt + kBuildInterval, std::min(newest + kSettle, oldest + 4 * kSettle));
    if (stale > 64 && stale > m_staticReal / 2)
      return m_builtAt + kBuildInterval;
    return ~0ull;
  }

  // The input comes a surface draw at a time (a run of one group): the runs
  // are sorted, and each run's emitters placed by rank. Where that does not
  // hold (a group in two runs, ranks that are not 0..n-1), every emitter is
  // sorted: least significant key first (identity, rank, group), a byte at a
  // time, then by type; every pass is stable.
  void LightBvh::sortOrder(const std::vector<SphereLight> &lights) {
    const size_t count = lights.size();
    m_order.resize(count);
    using Run = SortRun;
    m_runs.clear();
    for (uint32_t i = 0; i < count;) {
      uint32_t j = i + 1;
      if (lights[i].group)
        while (j < count && lights[j].group == lights[i].group && lights[j].triangle == lights[i].triangle)
          ++j;
      m_runs.push_back({lights[i].triangle, lights[i].group, lights[i].group ? 0 : lights[i].id, i, j - i});
      i = j;
    }
    std::sort(m_runs.begin(), m_runs.end(), [](const Run &a, const Run &b) {
      return std::tie(a.triangle, a.group, a.id) < std::tie(b.triangle, b.group, b.id);
    });
    bool placed = true;
    uint32_t out = 0;
    for (size_t r = 0; placed && r < m_runs.size(); ++r) {
      const auto &run = m_runs[r];
      if (r && run.group && run.group == m_runs[r - 1].group && run.triangle == m_runs[r - 1].triangle)
        placed = false; // one group in two runs
      for (uint32_t i = 0; placed && i < run.count; ++i) {
        const uint32_t rank = lights[run.first + i].rank;
        placed = rank < run.count && (run.group || rank == 0);
        if (placed)
          m_order[out + rank] = ~0u;
      }
      for (uint32_t i = 0; placed && i < run.count; ++i) {
        auto &slot = m_order[out + lights[run.first + i].rank];
        placed = slot == ~0u; // ranks are distinct
        slot = run.first + i;
      }
      out += run.count;
    }
    if (placed)
      return;
    std::iota(m_order.begin(), m_order.end(), 0u);
    m_sortKeys.resize(count);
    m_sortSpare.resize(count);
    const auto field = [&](auto value, unsigned bytes) {
      for (size_t i = 0; i < count; ++i)
        m_sortKeys[i] = {uint64_t(value(lights[m_order[i]])), m_order[i]};
      for (unsigned shift = 0; shift < 8 * bytes; shift += 8) {
        size_t counts[257] = {};
        for (const auto &key : m_sortKeys)
          ++counts[((key.first >> shift) & 255) + 1];
        if (std::find(std::begin(counts) + 1, std::end(counts), count) != std::end(counts))
          continue; // every key has this byte
        for (unsigned b = 1; b < 257; ++b)
          counts[b] += counts[b - 1];
        for (const auto &key : m_sortKeys)
          m_sortSpare[counts[(key.first >> shift) & 255]++] = key;
        m_sortKeys.swap(m_sortSpare);
      }
      for (size_t i = 0; i < count; ++i)
        m_order[i] = m_sortKeys[i].second;
    };
    field([](const SphereLight &light) { return light.id; }, 8);
    field([](const SphereLight &light) { return light.rank; }, 4);
    field([](const SphereLight &light) { return light.group; }, 8);
    field([](const SphereLight &light) { return light.triangle; }, 4);
  }

  void LightBvh::setClusterSize(uint32_t size) {
    size = std::max(size, 1u);
    if (size == m_clusterSize)
      return;
    m_clusterSize = size;
    m_nodes.clear(); // the next update builds (Initial)
    m_due = 0;
  }

  void LightBvh::makeClusters(uint32_t begin, uint32_t end) {
    for (uint32_t i = begin; i < end;) {
      uint32_t j = i + 1;
      const auto &first = m_lights[i];
      if (m_clusterSize > 1 && first.group)
        while (j < end && m_lights[j].group == first.group && m_lights[j].triangle == first.triangle)
          ++j;
      // Runs of a surface in nearly equal parts of at most m_clusterSize.
      const uint32_t run = j - i, parts = (run + m_clusterSize - 1) / m_clusterSize;
      for (uint32_t p = 0; p < parts; ++p) {
        const uint32_t from = i + uint32_t(uint64_t(run) * p / parts), to = i + uint32_t(uint64_t(run) * (p + 1) / parts);
        std::array<float, 3> center{};
        for (uint32_t e = from; e < to; ++e)
          for (unsigned c = 0; c < 3; ++c)
            center[c] += m_lights[e].position[c];
        for (auto &c : center)
          c /= float(to - from);
        m_clusters.emplace_back(from, to - from);
        m_centers.push_back(center);
      }
      i = j;
    }
  }

  bool LightBvh::update(const std::vector<SphereLight> &input) {
    const auto start = std::chrono::steady_clock::now();
    const uint64_t buildsBefore = builds;
    const auto finish = [&] {
      updateMilliseconds =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (builds != buildsBefore)
        buildTotalMilliseconds += buildMilliseconds = updateMilliseconds;
    };
    ++m_update;
    m_changing = generation + 1;
    auto phaseStart = start;
    const bool timed = sceneTimings();
    const auto mark = [&](unsigned phase) {
      if (!timed)
        return;
      const auto now = std::chrono::steady_clock::now();
      g_lightChurn.phases[phase] += std::chrono::duration<double, std::milli>(now - phaseStart).count();
      phaseStart = now;
    };
    ++g_lightChurn.updates;
    bool orderChanged = input.size() != m_sourceKeys.size();
    for (size_t i = 0; !orderChanged && i < input.size(); ++i)
      orderChanged = m_sourceKeys[i] != std::pair{input[i].id, input[i].triangle};
    if (orderChanged) {
      ++sorts;
      m_sourceKeys.resize(input.size());
      for (size_t i = 0; i < input.size(); ++i)
        m_sourceKeys[i] = {input[i].id, input[i].triangle};
      sortOrder(input);
    }
    mark(0);
    // In m_input, whose storage (like m_sorted's, swapped with it below) is
    // kept from update to update: megabytes with many emissive triangles.
    auto &lights = m_input;
    lights.resize(input.size());
    for (size_t i = 0; i < m_order.size(); ++i)
      lights[i] = input[m_order[i]];
    mark(1);
    // Sphere lights that changed since the last update, now and before (both
    // lists are sorted with sphere lights first, by identity).
    m_moved.clear();
    {
      auto zero = [](LightEmitter e) {
        std::fill_n(e.radiance, 3, 0.f);
        return e;
      };
      size_t a = 0, b = 0;
      while (m_moved.size() < kMaxMoved) {
        const bool hasA = a < lights.size() && lights[a].triangle == 0;
        const bool hasB = b < m_sorted.size() && m_sorted[b].triangle == 0;
        if (!hasA && !hasB)
          break;
        if (hasA && hasB && lights[a].id == m_sorted[b].id) {
          if (!(lights[a] == m_sorted[b]))
            m_moved.push_back({emitterOf(lights[a]), emitterOf(m_sorted[b])});
          ++a, ++b;
        } else if (hasA && (!hasB || lights[a].id < m_sorted[b].id)) {
          const auto e = emitterOf(lights[a++]); // switched on
          m_moved.push_back({e, zero(e)});
        } else {
          const auto e = emitterOf(m_sorted[b++]); // switched off
          m_moved.push_back({zero(e), e});
        }
      }
    }
    const bool unchanged = lights == m_sorted;
    mark(2);
    if (unchanged && m_update < m_due) {
      finish();
      return false;
    }
    if (lights.empty()) {
      m_nodes.clear();
      m_parents.clear();
      m_leaves.clear();
      m_emitters.clear();
      m_lights.clear();
      m_stamps.clear();
      m_present.clear();
      m_seen.clear();
      m_dynamicSeen.clear();
      m_split = false;
      m_staticReal = m_dynamicReal = m_staticCount = m_staticNodes = m_staticDepth = 0;
      active = depth = staticLights = dynamicLights = retiredSlots = unlitDynamicSlots = instanceLights = 0;
      m_due = ~0ull;
      m_builtAt = m_update;
      m_structure = m_changing;
      ++builds;
      ++buildReasons[lastBuildReason = Cleared];
      ++visibilityGeneration;
      significance(lights);
      m_sorted.clear();
      ++generation;
      finish();
      return true;
    }
    // Absent static lights, those among them gone for kRetain updates, and
    // the update the next of the others goes stale in (an unchanged input
    // skips the matching below until then).
    uint32_t stale = 0;
    uint64_t nextStale = ~0ull;
    Reason reason = m_nodes.empty() ? Initial : None;
    bool geometryChanged = false;
    // Lights without a static slot (indices into lights), merged with the
    // held dynamic ones: per dynamic slot to be, its light (an index into
    // lights, or kHeld | a held dynamic slot whose light is absent), the
    // update its geometry last changed in and the update it was last seen in.
    // Indices, not copies: with many emissive props this runs every frame.
    auto &extraIndex = m_extraIndex;
    auto &source = m_mergeSource;
    auto &extraStamps = m_mergeStamps;
    auto &extraSeen = m_mergeSeen;
    extraIndex.clear();
    source.clear();
    extraStamps.clear();
    extraSeen.clear();
    constexpr uint32_t kHeld = 0x80000000u;
    const SphereLight *heldDynamic = m_lights.data() + m_staticCount;
    const auto at = [&](uint32_t k) -> const SphereLight & {
      return source[k] & kHeld ? heldDynamic[source[k] & ~kHeld] : lights[source[k]];
    };
    // The merged dynamic lights as copies (absent ones unlit), for builds.
    const auto materialize = [&] {
      std::vector<SphereLight> out;
      out.reserve(source.size());
      for (uint32_t k = 0; k < source.size(); ++k) {
        out.push_back(at(k));
        if (source[k] & kHeld)
          std::fill_n(out.back().radiance, 3, 0.f);
      }
      return out;
    };
    bool sameKeys = true;
    uint64_t added = 0, addedSurfaces = 0;
    uint64_t oldest = ~0ull, newest = 0;
    uint32_t retired = 0, unlitDynamic = 0; // absent static and dynamic lights
    if (reason == None) {
      m_dirty.assign(m_nodes.size(), 0);
      // Static slots are in emitter order, as the input: one pass matches them.
      size_t in = 0;
      for (uint32_t slot = 0; slot < m_staticReal; ++slot) {
        auto &held = m_lights[slot];
        const auto key = keyOf(held);
        while (in < lights.size() && keyOf(lights[in]) < key)
          extraIndex.push_back(uint32_t(in++));
        if (in < lights.size() && keyOf(lights[in]) == key) {
          const auto &light = lights[in++];
          m_seen[slot] = m_update;
          if (!m_present[slot] || !(light == held)) {
            if (!sameGeometry(light, held)) {
              m_stamps[slot] = m_update;
              geometryChanged = true;
            }
            held = light;
            m_emitters[slot] = emitterOf(light);
            m_dirty[m_leaves[slot]] = 1;
            m_present[slot] = 1;
          }
        } else {
          // Gone: the slot stays, unlit, for the light's return.
          if (m_present[slot]) {
            m_present[slot] = 0;
            std::fill_n(held.radiance, 3, 0.f);
            m_emitters[slot] = emitterOf(held);
            m_dirty[m_leaves[slot]] = 1;
          }
          ++retired;
          if (m_update - m_seen[slot] >= kRetain)
            ++stale;
          else
            nextStale = std::min(nextStale, m_seen[slot] + kRetain);
        }
      }
      mark(3);
      for (; in < lights.size(); ++in)
        extraIndex.push_back(uint32_t(in));
      // Dynamic lights are matched to the held ones by key; absent ones keep
      // their slot, unlit, for kRetain updates (props leaving and entering
      // the view must not rebuild the dynamic part each time). Slots gone
      // longer are dropped only with a rebuild that happens anyway, or once
      // they are a quarter of the part.
      const size_t capacity = std::max<size_t>(extraIndex.size(), m_dynamicReal);
      source.reserve(capacity);
      extraStamps.reserve(capacity);
      extraSeen.reserve(capacity);
      uint64_t addedGroup = 0, staleDynamic = 0;
      std::vector<uint32_t> staleSlots;
      for (size_t i = 0, d = 0; i < extraIndex.size() || d < m_dynamicReal;) {
        const bool hasExtra = i < extraIndex.size(), hasHeld = d < m_dynamicReal;
        const SphereLight *light = hasExtra ? &lights[extraIndex[i]] : nullptr;
        if (hasExtra && hasHeld && keyOf(*light) == keyOf(heldDynamic[d])) {
          source.push_back(extraIndex[i]);
          extraStamps.push_back(sameGeometry(*light, heldDynamic[d]) ? m_stamps[m_staticCount + d] : m_update);
          extraSeen.push_back(m_update);
          ++i, ++d;
        } else if (hasExtra && (!hasHeld || keyOf(*light) < keyOf(heldDynamic[d]))) {
          source.push_back(extraIndex[i]);
          extraStamps.push_back(m_update);
          extraSeen.push_back(m_update);
          sameKeys = false;
          ++added;
          addedSurfaces += light->group != addedGroup;
          addedGroup = light->group;
          ++i;
        } else {
          if (m_update - m_dynamicSeen[d] >= kRetain) {
            ++staleDynamic;
            staleSlots.push_back(uint32_t(source.size()));
          }
          source.push_back(kHeld | uint32_t(d));
          extraStamps.push_back(m_stamps[m_staticCount + d]);
          extraSeen.push_back(m_dynamicSeen[d]);
          ++unlitDynamic;
          ++d;
        }
      }
      mark(4);
      if (!sameKeys || 4 * staleDynamic > source.size()) {
        // Rebuilding: drop the stale slots with it.
        g_lightChurn.removed += staleSlots.size();
        unlitDynamic -= uint32_t(staleSlots.size());
        sameKeys &= staleSlots.empty();
        size_t out = 0;
        for (size_t i = 0, k = 0; i < source.size(); ++i) {
          if (k < staleSlots.size() && staleSlots[k] == i) {
            ++k;
            continue;
          }
          source[out] = source[i];
          extraStamps[out] = extraStamps[i];
          extraSeen[out] = extraSeen[i];
          ++out;
        }
        source.resize(out);
        extraStamps.resize(out);
        extraSeen.resize(out);
      }
      for (uint32_t k = 0; k < source.size(); ++k)
        if (!at(k).dynamic) {
          oldest = std::min(oldest, extraStamps[k]);
          newest = std::max(newest, extraStamps[k]);
        }
      if (!source.empty() && !m_split)
        reason = FirstDynamic; // the hierarchy gains its dynamic part
      else if (m_update >= due(oldest, newest, stale))
        reason = oldest != ~0ull ? Settled : Retired;
      if (reason == None && unchanged) {
        m_due = std::min(due(oldest, newest, stale), nextStale);
        finish();
        return false;
      }
      if (reason == None) {
        if (sameKeys) {
          for (uint32_t k = 0; k < source.size(); ++k) {
            const size_t slot = m_staticCount + k;
            auto &kept = m_lights[slot];
            if (source[k] & kHeld) {
              // Absent: unlit in its slot.
              if (kept.radiance[0] != 0 || kept.radiance[1] != 0 || kept.radiance[2] != 0) {
                std::fill_n(kept.radiance, 3, 0.f);
                m_emitters[slot] = emitterOf(kept);
                m_dirty[m_leaves[slot]] = 1;
              }
              continue;
            }
            const auto &light = lights[source[k]];
            if (light == kept)
              continue;
            geometryChanged |= extraStamps[k] != m_stamps[slot];
            kept = light;
            m_stamps[slot] = extraStamps[k];
            m_emitters[slot] = emitterOf(light);
            m_dirty[m_leaves[slot]] = 1;
          }
        } else {
          ++g_lightChurn.rebuilds;
          g_lightChurn.added += added;
          g_lightChurn.addedSurfaces += addedSurfaces;
          auto merged = materialize();
          m_lights.resize(m_staticCount);
          m_stamps.resize(m_staticCount);
          m_dynamicReal = uint32_t(merged.size());
          m_lights.insert(m_lights.end(), merged.begin(), merged.end());
          m_stamps.insert(m_stamps.end(), extraStamps.begin(), extraStamps.end());
          buildDynamic();
          ++dynamicBuilds;
          geometryChanged = true;
        }
        m_dynamicSeen.swap(extraSeen);
        mark(5);
        for (size_t i = m_nodes.size(); i-- > 0;)
          if (m_dirty[i]) {
            bounds(uint32_t(i));
            if (i)
              m_dirty[m_parents[i]] = 1;
          }
        ++refits;
        mark(6);
      }
    }
    if (reason != None) {
      // Lights at rest go to the static part; those of dynamic instances and
      // those that appeared or moved lately form the dynamic part.
      // Absent lights not yet stale keep their slots (unlit, at rest).
      struct Placed {
        SphereLight light;
        uint64_t stamp, seen;
      };
      std::vector<Placed> statics, dynamics;
      const auto place = [&](const SphereLight &light, uint64_t stamp, uint64_t seen) {
        (!light.dynamic && m_update - stamp >= kSettle ? statics : dynamics).push_back({light, stamp, seen});
      };
      if (reason == Initial) {
        for (const auto &light : lights)
          place(light, light.dynamic ? m_update : m_update - kSettle, m_update);
      } else {
        for (uint32_t slot = 0; slot < m_staticReal; ++slot)
          if (m_present[slot])
            place(m_lights[slot], m_stamps[slot], m_update);
          else if (m_update - m_seen[slot] < kRetain)
            statics.push_back({m_lights[slot], m_stamps[slot], m_seen[slot]});
        const auto extra = materialize();
        for (size_t i = 0; i < extra.size(); ++i)
          place(extra[i], extraStamps[i], extraSeen[i]);
        const auto byKey = [](const auto &a, const auto &b) { return keyOf(a.light) < keyOf(b.light); };
        std::stable_sort(statics.begin(), statics.end(), byKey);
        std::stable_sort(dynamics.begin(), dynamics.end(), byKey);
      }
      std::vector<SphereLight> parts[2];
      std::vector<uint64_t> stamps[2], seen[2];
      for (unsigned part = 0; part < 2; ++part)
        for (auto &placed : part ? dynamics : statics) {
          parts[part].push_back(placed.light);
          stamps[part].push_back(placed.stamp);
          seen[part].push_back(placed.seen);
        }
      build(std::move(parts[0]), std::move(stamps[0]), std::move(seen[0]), std::move(parts[1]),
            std::move(stamps[1]), std::move(seen[1]), reason);
      geometryChanged = true;
      retired = stale = 0;
      nextStale = ~0ull;
      for (uint32_t slot = 0; slot < m_staticReal; ++slot)
        if (!m_present[slot]) {
          ++retired;
          nextStale = std::min(nextStale, m_seen[slot] + kRetain);
        }
      oldest = ~0ull, newest = 0;
      for (uint32_t i = 0; i < m_dynamicReal; ++i)
        if (!m_lights[m_staticCount + i].dynamic) {
          oldest = std::min(oldest, m_stamps[m_staticCount + i]);
          newest = std::max(newest, m_stamps[m_staticCount + i]);
        }
    }
    if (geometryChanged)
      ++visibilityGeneration;
    m_due = std::min(due(oldest, newest, stale), nextStale);
    mark(7);
    active = instanceLights = 0;
    for (const auto &light : m_lights)
      active += lit(light);
    for (const auto &light : lights)
      instanceLights += light.dynamic;
    if (reason != None) {
      unlitDynamic = 0;
      for (const auto seen : m_dynamicSeen)
        unlitDynamic += seen != m_update;
    }
    retiredSlots = retired;
    unlitDynamicSlots = unlitDynamic;
    g_lightChurn.unlit = unlitDynamic;
    staticLights = m_staticReal - retired;
    dynamicLights = m_dynamicReal - unlitDynamic;
    mark(8);
    significance(lights);
    m_sorted.swap(lights);
    mark(9);
    ++generation;
    finish();
    return true;
  }

  // Tiny animated-emission fluctuations still update the GPU light data, but
  // must not keep every local-light filter/cache permanently in fast response.
  // Compare against the last significant snapshot, not the preceding frame,
  // so gradual ramps eventually cross the threshold too. A light that moved,
  // reshaped, appeared or went counts with its whole power: one animated or
  // carried light among many (a blinking siren, a bot's flashlight) must not
  // reset the local denoiser every frame, while a dominant one still does.
  // This is a scene-wide fast-response switch, not a per-emitter filter.
  void LightBvh::significance(const std::vector<SphereLight> &sorted) {
    const auto &before = m_temporalLights;
    double changed = 0, totalOld = 0, totalNew = 0;
    const auto energy = [](const SphereLight &light) {
      return (double(light.radiance[0]) + light.radiance[1] + light.radiance[2]) * powerScale(light);
    };
    // Energies of the input (kept with it if it becomes the snapshot) and of
    // the snapshot: one powerScale per light.
    auto &energies = m_energyScratch;
    energies.resize(sorted.size());
    size_t a = 0, b = 0;
    while (a < sorted.size() || b < before.size()) {
      if (a < sorted.size() && b < before.size() && keyOf(sorted[a]) == keyOf(before[b])) {
        const auto &now = sorted[a], &then = before[b];
        const double newEnergy = energies[a++] = energy(now), oldEnergy = m_temporalEnergies[b++];
        double difference = 0;
        for (unsigned c = 0; c < 3; ++c)
          difference += std::abs(double(now.radiance[c]) - then.radiance[c]);
        const bool moved = (oldEnergy == 0) != (newEnergy == 0) || !sameGeometry(now, then);
        changed += moved ? std::max(oldEnergy, newEnergy)
                         : newEnergy > 0 ? difference * newEnergy / (double(now.radiance[0]) + now.radiance[1] + now.radiance[2])
                                         : 0;
        totalOld += oldEnergy;
        totalNew += newEnergy;
      } else if (a < sorted.size() && (b == before.size() || keyOf(sorted[a]) < keyOf(before[b]))) {
        const double value = energies[a] = energy(sorted[a]);
        ++a;
        changed += value;
        totalNew += value;
      } else {
        const double value = m_temporalEnergies[b++];
        changed += value;
        totalOld += value;
      }
    }
    if (sorted.empty() != before.empty() || changed > .25 * std::max(totalOld, totalNew)) {
      ++lightingGeneration;
      ++g_lightChurn.snapshots;
      m_temporalLights = sorted;
      m_temporalEnergies.swap(energies);
    }
  }

  // Nodes are laid out with both children of a node adjacent: the descent in
  // light_bvh.glsl reads both at every level, so one memory neighborhood per
  // level instead of a far jump to every right subtree. With a dynamic part
  // the root's children are the two parts (nodes 1 and 2), the static part's
  // nodes follow, and the dynamic part's come last; leaf ranks and emitters
  // are in the same order (static, then dynamic).
  void LightBvh::build(std::vector<SphereLight> statics, std::vector<uint64_t> staticStamps,
                       std::vector<uint64_t> staticSeen, std::vector<SphereLight> dynamics,
                       std::vector<uint64_t> dynamicStamps, std::vector<uint64_t> dynamicSeen, Reason reason) {
    m_split |= !dynamics.empty();
    m_staticReal = uint32_t(statics.size());
    m_dynamicReal = uint32_t(dynamics.size());
    m_lights = std::move(statics);
    m_stamps = std::move(staticStamps);
    if (m_split && m_lights.empty()) {
      m_lights.emplace_back();
      m_stamps.push_back(0);
    }
    m_staticCount = uint32_t(m_lights.size());
    m_present.assign(m_staticCount, 1);
    m_seen = std::move(staticSeen);
    m_seen.resize(m_staticCount, m_update);
    for (uint32_t slot = 0; slot < m_staticReal; ++slot)
      m_present[slot] = m_seen[slot] == m_update;
    m_lights.insert(m_lights.end(), dynamics.begin(), dynamics.end());
    m_stamps.insert(m_stamps.end(), dynamicStamps.begin(), dynamicStamps.end());
    m_dynamicSeen = std::move(dynamicSeen);
    if (m_split && dynamics.empty()) {
      m_lights.emplace_back();
      m_stamps.push_back(0);
    }
    const uint32_t total = uint32_t(m_lights.size());
    m_emitters.resize(total);
    for (uint32_t i = 0; i < total; ++i)
      m_emitters[i] = emitterOf(m_lights[i]);
    m_clusters.clear();
    m_centers.clear();
    makeClusters(0, m_staticCount);
    m_staticClusters = uint32_t(m_clusters.size());
    makeClusters(m_staticCount, total);
    const uint32_t leaves = uint32_t(m_clusters.size());
    m_nodes.clear();
    m_parents.clear();
    m_leaves.assign(total, 0);
    m_nodes.reserve(2 * size_t(leaves) - 1);
    depth = 0;
    m_splitDepthLimit = 2 * uint32_t(std::ceil(std::log2(std::max(leaves, 1u)))) + 2;
    std::vector<uint32_t> order(leaves);
    std::iota(order.begin(), order.end(), 0u);
    if (!m_split) {
      m_nodes.emplace_back();
      m_parents.push_back(0);
      buildInto(0, order, 0, leaves, 1);
      m_staticNodes = uint32_t(m_nodes.size());
      m_staticDepth = depth;
    } else {
      m_nodes.resize(3);
      m_parents.assign(3, 0);
      buildInto(1, order, 0, m_staticClusters, 2);
      m_staticNodes = uint32_t(m_nodes.size());
      m_staticDepth = depth;
      buildInto(2, order, m_staticClusters, leaves, 2);
      m_nodes[0].links[0] = 1;
      m_nodes[0].links[1] = 2;
      m_nodes[0].links[2] = 0;
      m_nodes[0].links[3] = leaves;
      bounds(0);
    }
    m_builtAt = m_update;
    m_structure = m_changing;
    ++builds;
    ++buildReasons[lastBuildReason = reason];
  }

  // The dynamic part again, from m_lights behind the static emitters. Static
  // nodes, emitters and leaf ranks stay as they are.
  void LightBvh::buildDynamic() {
    m_structure = m_changing;
    if (m_lights.size() == m_staticCount) {
      m_lights.emplace_back();
      m_stamps.push_back(0);
    }
    const uint32_t total = uint32_t(m_lights.size());
    m_emitters.resize(total);
    for (uint32_t i = m_staticCount; i < total; ++i)
      m_emitters[i] = emitterOf(m_lights[i]);
    m_clusters.resize(m_staticClusters);
    m_centers.resize(m_staticClusters);
    makeClusters(m_staticCount, total);
    const uint32_t leaves = uint32_t(m_clusters.size());
    m_nodes.resize(m_staticNodes);
    m_parents.resize(m_staticNodes);
    m_leaves.resize(total);
    m_nodes[2] = {};
    depth = m_staticDepth;
    m_splitDepthLimit = 2 * uint32_t(std::ceil(std::log2(std::max(leaves, 1u)))) + 2;
    std::vector<uint32_t> order(leaves);
    std::iota(order.begin(), order.end(), 0u);
    buildInto(2, order, m_staticClusters, leaves, 2);
    m_nodes[0].links[3] = leaves;
    m_dirty.resize(m_nodes.size());
    std::fill(m_dirty.begin() + m_staticNodes, m_dirty.end(), 0);
    m_dirty[0] = 1;
  }

  void LightBvh::buildInto(uint32_t index, std::vector<uint32_t> &order, uint32_t begin, uint32_t end,
                           uint32_t level) {
    depth = std::max(depth, level);
    m_nodes[index].links[3] = end - begin;
    // Subtrees are not contiguous in node order (siblings are adjacent), so
    // each node records the first position of its leaves in leaf order
    // (links[0] of a leaf, links[2] of an internal node): subtree membership
    // is a leaf-rank range test (light_guiding.glsl lightRank).
    if (end - begin == 1) {
      const auto [first, count] = m_clusters[order[begin]];
      m_nodes[index].links[0] = begin;
      m_nodes[index].links[1] = count;
      m_nodes[index].links[2] = first;
      for (uint32_t e = first; e < first + count; ++e)
        m_leaves[e] = index;
    } else {
      float minimum[3], maximum[3];
      std::fill_n(minimum, 3, std::numeric_limits<float>::max());
      std::fill_n(maximum, 3, std::numeric_limits<float>::lowest());
      for (uint32_t i = begin; i < end; ++i)
        for (unsigned c = 0; c < 3; ++c) {
          minimum[c] = std::min(minimum[c], m_centers[order[i]][c]);
          maximum[c] = std::max(maximum[c], m_centers[order[i]][c]);
        }
      unsigned axis = 0;
      for (unsigned c = 1; c < 3; ++c)
        if (maximum[c] - minimum[c] > maximum[axis] - minimum[axis])
          axis = c;
      uint32_t middle = begin + (end - begin) / 2;
      const auto byAxis = [&](unsigned a) {
        return [&, a](uint32_t x, uint32_t y) {
          return m_centers[x][a] != m_centers[y][a] ? m_centers[x][a] < m_centers[y][a] : x < y;
        };
      };
      // The count median on the widest axis keeps the tree balanced, but it
      // can keep a small nearby light with huge distant emitters (a 3D
      // skybox's lit windows) for several levels, where the shared box's
      // distance regularization starves it; so do a few lights far below a
      // map, stretching a box 16000 units. Take a surface area heuristic
      // (by perimeter) split instead when it is at least a fifth cheaper,
      // above a depth bound (uniform grids keep the median up to about .85).
      // It reads only the emitters' positions and counts, which refits keep:
      // a refit must match a rebuild (power and radius change by refit).
      if (level < m_splitDepthLimit) {
        std::vector<Extent> extents(end - begin), suffix(end - begin + 1);
        for (uint32_t i = begin; i < end; ++i) {
          auto &extent = extents[i - begin];
          const auto [first, count] = m_clusters[order[i]];
          for (uint32_t e = first; e < first + count; ++e) {
            const auto &light = m_lights[e];
            for (unsigned c = 0; c < 3; ++c) {
              extent.minimum[c] = std::min(extent.minimum[c], light.position[c]);
              extent.maximum[c] = std::max(extent.maximum[c], light.position[c]);
            }
          }
          extent.count = count;
        }
        const auto cost = [&](const Extent &e) { return e.count * e.size(); };
        double medianCost = 0, bestCost = std::numeric_limits<double>::max();
        unsigned bestAxis = axis;
        uint32_t bestSplit = middle;
        std::vector<uint32_t> sorted(order.begin() + begin, order.begin() + end), local(end - begin);
        for (unsigned a = 0; a < 3; ++a) {
          std::iota(local.begin(), local.end(), 0u);
          std::sort(local.begin(), local.end(), [&](uint32_t x, uint32_t y) {
            return byAxis(a)(sorted[x], sorted[y]);
          });
          suffix[end - begin] = {};
          for (uint32_t i = end - begin; i-- > 0;) {
            suffix[i] = suffix[i + 1];
            suffix[i].add(extents[local[i]]);
          }
          Extent prefix;
          for (uint32_t i = 1; i < end - begin; ++i) {
            prefix.add(extents[local[i - 1]]);
            const double c = cost(prefix) + cost(suffix[i]);
            if (a == axis && begin + i == middle)
              medianCost = c;
            if (c < bestCost) {
              bestCost = c;
              bestAxis = a;
              bestSplit = begin + i;
            }
          }
        }
        if (bestCost < .8 * medianCost) {
          axis = bestAxis;
          middle = bestSplit;
        }
      }
      if (middle == begin + (end - begin) / 2)
        std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end, byAxis(axis));
      else
        std::sort(order.begin() + begin, order.begin() + end, byAxis(axis));
      const uint32_t left = uint32_t(m_nodes.size()), right = left + 1;
      m_nodes.emplace_back();
      m_nodes.emplace_back();
      m_parents.push_back(index);
      m_parents.push_back(index);
      m_nodes[index].links[0] = left;
      m_nodes[index].links[1] = right;
      m_nodes[index].links[2] = begin;
      buildInto(left, order, begin, middle, level + 1);
      buildInto(right, order, middle, end, level + 1);
    }
    bounds(index);
  }

  bool LightBvh::changedSince(uint64_t instance, uint64_t since, Ranges &emitters, Ranges &nodes) const {
    emitters.clear();
    nodes.clear();
    if (instance != m_instance || since < m_structure || since > generation ||
        m_emitterChanged.size() < m_emitters.size() || m_nodeChanged.size() < m_nodes.size())
      return false;
    // Ranges of entries changed after `since`, joined across gaps of up to
    // kJoin unchanged ones (fewer, larger copies).
    constexpr uint32_t kJoin = 8;
    const auto collect = [&](const std::vector<uint64_t> &changed, uint32_t count, Ranges &out) {
      for (uint32_t i = 0; i < count; ++i)
        if (changed[i] > since) {
          if (!out.empty() && i - out.back().second <= kJoin)
            out.back().second = i + 1;
          else
            out.emplace_back(i, i + 1);
        }
    };
    collect(m_emitterChanged, uint32_t(m_emitters.size()), emitters);
    collect(m_nodeChanged, uint32_t(m_nodes.size()), nodes);
    return true;
  }

  void LightBvh::bounds(uint32_t index) {
    auto &node = m_nodes[index];
    if (m_nodeChanged.size() < m_nodes.size())
      m_nodeChanged.resize(m_nodes.size());
    m_nodeChanged[index] = m_changing;
    if (node.links[3] == 1) {
      if (m_emitterChanged.size() < m_emitters.size())
        m_emitterChanged.resize(m_emitters.size());
      std::fill_n(m_emitterChanged.begin() + node.links[2], std::max(node.links[1], 1u), m_changing);
      // A leaf's emitters: bounds of the lit ones (all when none is, as for
      // internal nodes below), summed power, and each one's share of it.
      const uint32_t first = node.links[2], count = std::max(node.links[1], 1u);
      const auto power = lightPower;
      double total = 0;
      for (uint32_t e = first; e < first + count; ++e)
        total += power(m_lights[e]);
      std::fill_n(node.minimumPower, 3, std::numeric_limits<float>::max());
      std::fill_n(node.maximum, 4, std::numeric_limits<float>::lowest());
      double running = 0;
      for (uint32_t e = first; e < first + count; ++e) {
        const auto &light = m_lights[e];
        const double own = power(light);
        if (own > 0 || total == 0)
          for (unsigned c = 0; c < 3; ++c) {
            node.minimumPower[c] = std::min(node.minimumPower[c], light.position[c] - light.radius);
            node.maximum[c] = std::max(node.maximum[c], light.position[c] + light.radius);
          }
        node.maximum[3] = std::max(node.maximum[3], light.radius * light.radius);
        const double share = total > 0 ? own / total : 1. / count;
        running += share;
        m_emitters[e].shaping[2] = e + 1 == first + count ? 1.f : float(running);
        m_emitters[e].shaping[3] = float(share);
      }
      node.minimumPower[3] = float(total);
    } else {
      const auto &a = m_nodes[node.links[0]], &b = m_nodes[node.links[1]];
      if (a.minimumPower[3] == 0 || b.minimumPower[3] == 0) {
        // Keep zero-power slots for cheap refits without expanding the live
        // influence bounds around distant unlit redstone torches.
        const auto &live = a.minimumPower[3] > 0 ? a : b;
        std::copy_n(live.minimumPower, 3, node.minimumPower);
        std::copy_n(live.maximum, 4, node.maximum);
      } else {
        for (unsigned c = 0; c < 3; ++c) {
          node.minimumPower[c] = std::min(a.minimumPower[c], b.minimumPower[c]);
          node.maximum[c] = std::max(a.maximum[c], b.maximum[c]);
        }
        node.maximum[3] = std::max(a.maximum[3], b.maximum[3]);
      }
      node.minimumPower[3] = a.minimumPower[3] + b.minimumPower[3];
    }
  }

  double LightBvh::falloff(double distanceSquared, double range) {
    if (range <= 0)
      return 1;
    const auto t = std::clamp((distanceSquared / (range * range) - .5625) * (16. / 7.), 0., 1.);
    return (1 - t) * (1 - t) * (1 + 2 * t);
  }

  bool LightBvh::save(const char *path) const {
    FILE *out = std::fopen(path, "wb");
    if (!out)
      return false;
    const uint32_t counts[2] = {uint32_t(m_nodes.size()), uint32_t(m_emitters.size())};
    const bool written = std::fwrite(counts, sizeof(counts), 1, out) == 1 &&
                         std::fwrite(m_nodes.data(), sizeof(LightNode), m_nodes.size(), out) == m_nodes.size() &&
                         std::fwrite(m_emitters.data(), sizeof(LightEmitter), m_emitters.size(), out) == m_emitters.size();
    return std::fclose(out) == 0 && written;
  }

  double LightBvh::weight(const LightNode &node, const float position[3], const float normal[3],
                          double range) {
    double distance2 = 0, radius2 = 0, toward = 0, reach = 0, nearest2 = 0;
    for (unsigned c = 0; c < 3; ++c) {
      const double half = .5 * (node.maximum[c] - node.minimumPower[c]);
      const double delta = .5 * (node.maximum[c] + node.minimumPower[c]) - position[c];
      distance2 += delta * delta;
      radius2 += half * half;
      toward += delta * normal[c];
      reach += half * std::abs(normal[c]);
      const double nearest = std::max(std::abs(delta) - half, 0.);
      nearest2 += nearest * nearest;
    }
    // Cull only when the entire box is below the receiving plane
    // (light_bvh.glsl).
    if (toward + reach <= 0)
      return 0;
    const double cosine =
        std::min(1., (toward + std::sqrt(radius2)) / std::sqrt(std::max({distance2, radius2, 1.e-12})));
    // Nearest-box times center distance (light_bvh.glsl).
    const double regularized = std::max(std::sqrt(nearest2 * distance2), .01 * radius2);
    return falloff(nearest2, range) * node.minimumPower[3] * cosine /
           std::max({regularized, double(node.maximum[3]), 1.e-12});
  }
} // namespace remix::scene
