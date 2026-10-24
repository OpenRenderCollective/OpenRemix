#include "scene/static_scene.h"
#include "platform/workers.h"
#include "scene/resources.h"
#include <remix/remix_native_ext.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>

namespace remix::scene {
  namespace {
    bool singularModel(const Matrix &m) {
      const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                        m[8] * (m[1] * m[6] - m[5] * m[2]);
      return std::abs(det) < 1e-12f;
    }
  } // namespace

  uint32_t StaticScene::allocate() {
    if (!m_free.empty()) {
      const uint32_t slot = m_free.back();
      m_free.pop_back();
      return slot;
    }
    m_units.emplace_back();
    return uint32_t(m_units.size() - 1);
  }

  void StaticScene::removeUnit(uint32_t slot) {
    auto &unit = m_units[slot];
    if (!unit.live)
      return;
    if (auto found = m_keys.find({unit.id, unit.index}); found != m_keys.end()) {
      m_duplicates -= found->second > 1;
      if (!--found->second)
        m_keys.erase(found);
    }
    if (auto found = m_ids.find(unit.id); found != m_ids.end() && !--found->second)
      m_ids.erase(found);
    if (!unit.singular) {
      --m_tracedUnits;
      m_tracedSurfaces -= unit.surfaces.size();
    }
    m_liveSurfaces -= unit.surfaces.size();
    m_livePbr -= unit.pbrSurfaces;
    m_removed.push_back({slot, unit.id, unit.index});
    m_emittingDirty |= !unit.emitters.empty();
    ++m_statistics.removedUnits;
    unit = {};
    m_freeing.push_back(slot);
    m_orderDirty = true;
  }

  void StaticScene::remove(uint64_t handle) {
    const auto found = m_handles.find(handle);
    if (found == m_handles.end())
      return;
    for (const uint32_t slot : found->second)
      removeUnit(slot);
    m_handles.erase(found);
  }

  void StaticScene::build(const RetainedInstance &entry, SurfaceEmissionCache &emission,
                          const NativeResources *resources) {
    auto &slots = m_handles[entry.handle];
    const uint32_t count = entry.instanceCount();
    const uint32_t surfaces = entry.surfaceCount;
    while (slots.size() > count) {
      removeUnit(slots.back());
      slots.pop_back();
    }
    for (uint32_t i = 0; i < count; ++i) {
      const bool reuse = i < slots.size();
      const uint32_t slot = reuse ? slots[i] : allocate();
      if (!reuse)
        slots.push_back(slot);
      auto &unit = m_units[slot];
      if (reuse) {
        // Rebuilt in place: the key is the handle's, so the order stays.
        if (!unit.singular) {
          --m_tracedUnits;
          m_tracedSurfaces -= unit.surfaces.size();
        }
        m_liveSurfaces -= unit.surfaces.size();
        m_livePbr -= unit.pbrSurfaces;
      } else {
        m_orderDirty = true;
      }
      const auto *records = entry.records.data() + size_t(i) * surfaces;
      unit.handle = entry.handle;
      unit.id = records[0].instanceId;
      unit.index = records[0].instanceIndex;
      unit.generation = ++m_generation;
      unit.live = true;
      unit.singular = singularModel(records[0].model);
      unit.surfaces.clear();
      unit.surfaces.reserve(surfaces);
      unit.pbrSurfaces = 0;
      m_emittingDirty |= !unit.emitters.empty();
      unit.emitters.clear();
      for (uint32_t s = 0; s < surfaces; ++s) {
        auto draw = records[s];
        if (resources)
          resources->applyDrawHidden(draw);
        TraceFrame::Surface surface{draw};
        // As prepareTraceFrame: paint only where blending is on.
        surface.draw.decal = draw.decal && draw.blend.enabled && !draw.material->transmission;
        const auto &material = *draw.material;
        // Eligible materials are not animated: frame 0 at any time.
        const auto animation = uint32_t(std::fmod(0.0 * material.fps, uint32_t(material.rows) * material.columns));
        surface.animationFrame = animation;
        surface.uv = {1.f / material.columns, 1.f / material.rows,
                      float(animation % material.columns) / material.columns,
                      float(animation / material.columns) / material.rows};
        unit.pbrSurfaces += material.pbr;
        unit.surfaces.push_back(std::move(surface));
        // Emitters, as prepareTraceFrame makes them (none for a singular unit).
        const auto &prepared = unit.surfaces.back();
        if (unit.singular || material.suppressEmissionLight || (draw.alphaOverride && draw.compare == 0) ||
            (material.emission[0] <= 0 && material.emission[1] <= 0 && material.emission[2] <= 0))
          continue;
        const auto reduction = emission.reduce(prepared.draw, prepared.uv);
        std::vector<SphereLight> lights;
        triangleEmitters(prepared.draw, *reduction, lights);
        for (size_t i = 0; i < lights.size(); ++i)
          unit.emitters.push_back({lights[i], (*reduction)[i].layout, bool(material.emissionTexture)});
      }
      m_emittingDirty |= !unit.emitters.empty();
      if (!unit.singular) {
        ++m_tracedUnits;
        m_tracedSurfaces += unit.surfaces.size();
      }
      m_liveSurfaces += unit.surfaces.size();
      m_livePbr += unit.pbrSurfaces;
      if (!reuse) {
        m_duplicates += m_keys[{unit.id, unit.index}]++ != 0;
        ++m_ids[unit.id];
      }
      if (!unit.pending) {
        unit.pending = true;
        m_rebuilt.push_back(slot);
      }
      m_partitionPending.push_back(slot);
      ++m_statistics.rebuiltUnits;
    }
    if (slots.empty())
      m_handles.erase(entry.handle);
  }

  bool StaticScene::sync(NativeResources &resources, OpacityCache &opacity, SurfaceEmissionCache &emission,
                         WorkerPool *workers) {
    if (!resources.staticDrawn())
      return false;
    const auto start = std::chrono::steady_clock::now();
    resources.takeStaticChanges(m_changes);
    m_statistics.changes += m_changes.size();
    if (resources.staticEpoch() != m_epoch) {
      // A new set (or a new consumer): rebuild every unit from the members.
      for (auto &[handle, slots] : m_handles)
        for (const uint32_t slot : slots)
          removeUnit(slot);
      m_handles.clear();
      m_epoch = resources.staticEpoch();
      resources.forEachStatic([&](const RetainedInstance &entry) { build(entry, emission, &resources); });
      ++m_statistics.resyncs;
    } else {
      for (const uint64_t handle : m_changes) {
        const auto *entry = resources.retained(handle);
        if (entry && entry->member)
          build(*entry, emission, &resources);
        else
          remove(handle);
      }
    }
    // Opacity partitions of the (re)built units: cached ones found on workers,
    // the rest proved serially, as prepareScene does for the frame's draws.
    if (!m_partitionPending.empty()) {
      std::sort(m_partitionPending.begin(), m_partitionPending.end());
      m_partitionPending.erase(std::unique(m_partitionPending.begin(), m_partitionPending.end()),
                               m_partitionPending.end());
      std::erase_if(m_partitionPending, [&](uint32_t slot) { return !m_units[slot].live; });
      auto find = [&](size_t i) {
        for (auto &surface : m_units[m_partitionPending[i]].surfaces)
          surface.opacity = opacity.find(surface.draw, surface.uv);
      };
      if (workers && m_partitionPending.size() > 64)
        workers->run(m_partitionPending.size(), find);
      else
        for (size_t i = 0; i < m_partitionPending.size(); ++i)
          find(i);
      for (const uint32_t slot : m_partitionPending)
        for (auto &surface : m_units[slot].surfaces)
          if (!surface.opacity)
            surface.opacity = opacity.partition(surface.draw, surface.uv);
      m_partitionPending.clear();
    }
    if (m_orderDirty)
      sort();
    if (m_emittingDirty) {
      m_emitting.clear();
      for (const uint32_t slot : m_order)
        if (!m_units[slot].emitters.empty())
          m_emitting.push_back(slot);
      m_emittingDirty = false;
    }
    m_statistics.units = m_tracedUnits;
    m_statistics.surfaces = m_tracedSurfaces;
    m_statistics.milliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
  }

  void StaticScene::sort() {
    // Surviving units keep their relative order; new ones are merged in.
    std::vector<uint32_t> kept, added;
    kept.reserve(m_order.size());
    std::vector<uint8_t> seen(m_units.size(), 0);
    for (const uint32_t slot : m_order)
      if (slot < m_units.size() && m_units[slot].live && !seen[slot]) {
        kept.push_back(slot);
        seen[slot] = 1;
      }
    for (uint32_t slot = 0; slot < m_units.size(); ++slot)
      if (m_units[slot].live && !seen[slot])
        added.push_back(slot);
    const auto less = [&](uint32_t a, uint32_t b) {
      const auto &x = m_units[a], &y = m_units[b];
      return x.id != y.id ? x.id < y.id : x.index < y.index;
    };
    std::sort(added.begin(), added.end(), less);
    m_order.clear();
    m_order.reserve(kept.size() + added.size());
    std::merge(kept.begin(), kept.end(), added.begin(), added.end(), std::back_inserter(m_order), less);
    m_orderDirty = false;
    m_orderChanged = true;
  }

  void StaticScene::acknowledge() {
    for (const uint32_t slot : m_rebuilt)
      m_units[slot].pending = false;
    m_rebuilt.clear();
    m_removed.clear();
    m_free.insert(m_free.end(), m_freeing.begin(), m_freeing.end());
    m_freeing.clear();
    m_orderChanged = false;
  }

  void StaticScene::clear() {
    m_units.clear();
    m_free.clear();
    m_freeing.clear();
    m_order.clear();
    m_rebuilt.clear();
    m_removed.clear();
    m_handles.clear();
    m_keys.clear();
    m_ids.clear();
    m_epoch = ~0ull;
    m_duplicates = 0;
    m_tracedUnits = m_tracedSurfaces = m_liveSurfaces = m_livePbr = 0;
    m_orderChanged = m_orderDirty = m_emittingDirty = false;
    m_emitting.clear();
    m_partitionPending.clear();
    m_timelines.clear();
    m_prunable.clear();
    m_absent.clear();
    m_committed = 0;
    m_hasCommitted = false;
  }

  void StaticScene::record(uint64_t id, uint32_t index, uint64_t serial,
                           std::shared_ptr<const StaticUnitState> state) {
    auto &timeline = m_timelines[{id, index}];
    const bool unused = !state;
    if (!timeline.empty() && timeline.back().first == serial)
      timeline.back().second = std::move(state);
    else
      timeline.emplace_back(serial, std::move(state));
    if (timeline.size() > 1 || unused)
      m_prunable.emplace_back(id, index);
  }

  void StaticScene::absent(uint64_t serial) {
    if (m_absent.empty() || m_absent.back() < serial)
      m_absent.push_back(serial);
  }

  void StaticScene::prune(std::pair<uint64_t, uint32_t> k, std::vector<std::shared_ptr<const void>> *retired) {
    const auto found = m_timelines.find(k);
    if (found == m_timelines.end())
      return;
    auto &timeline = found->second;
    // Keep the state covering the committed frame and everything after it.
    size_t first = 0;
    while (first + 1 < timeline.size() && timeline[first + 1].first <= m_committed)
      ++first;
    if (first && retired)
      for (size_t i = 0; i < first; ++i)
        if (timeline[i].second)
          retired->push_back(timeline[i].second);
    timeline.erase(timeline.begin(), timeline.begin() + first);
    if (timeline.size() == 1 && !timeline[0].second && timeline[0].first <= m_committed)
      m_timelines.erase(found);
  }

  void StaticScene::committed(uint64_t serial, std::vector<std::shared_ptr<const void>> &retired) {
    m_committed = serial;
    m_hasCommitted = true;
    m_absent.erase(m_absent.begin(), std::lower_bound(m_absent.begin(), m_absent.end(), serial));
    std::sort(m_prunable.begin(), m_prunable.end());
    m_prunable.erase(std::unique(m_prunable.begin(), m_prunable.end()), m_prunable.end());
    size_t keep = 0;
    for (const auto &k : m_prunable) {
      prune(k, &retired);
      const auto found = m_timelines.find(k);
      if (found != m_timelines.end() && (found->second.size() > 1 || !found->second.back().second))
        m_prunable[keep++] = k;
    }
    m_prunable.resize(keep);
  }

  bool StaticScene::committedAbsent() const {
    return m_hasCommitted && std::binary_search(m_absent.begin(), m_absent.end(), m_committed);
  }

  const StaticUnitState *StaticScene::committedState(uint64_t id, uint32_t index) const {
    if (!m_hasCommitted || committedAbsent())
      return nullptr;
    const auto found = m_timelines.find({id, index});
    if (found == m_timelines.end())
      return nullptr;
    const StaticUnitState *state = nullptr;
    for (const auto &[serial, value] : found->second) {
      if (serial > m_committed)
        break;
      state = value.get();
    }
    return state;
  }

  const TemporalHistory::SurfaceHistory *StaticScene::previous(const TemporalKey &key) const {
    const auto *state = committedState(std::get<0>(key), std::get<1>(key));
    const uint32_t surface = std::get<2>(key);
    return state && surface < state->surfaces.size() ? &state->surfaces[surface] : nullptr;
  }

  uint64_t StaticScene::previousVertices(const TemporalKey &key) const {
    const auto *state = committedState(std::get<0>(key), std::get<1>(key));
    const uint32_t surface = std::get<2>(key);
    return state && surface < state->vertices.size() ? state->vertices[surface] : 0;
  }

  std::vector<TraceFrame::Emitter> mergedEmitters(const TraceFrame &frame, const StaticScene *statics,
                                                  bool filterEmission) {
    // The set's instances are all static; keys never collide.
    if (frame.staticUnits && statics && !statics->emitting().empty()) {
      std::vector<TraceFrame::Emitter> merged;
      const auto &emitting = statics->emitting();
      const auto &all = statics->units();
      auto unitEmitters = [&](const StaticUnit &unit) {
        for (auto emitter : unit.emitters) {
          emitter.animated = filterEmission && emitter.animated;
          if (!emitter.animated)
            emitter.layout = 0;
          merged.push_back(emitter);
        }
      };
      size_t next = 0;
      for (size_t i = 0; i < frame.instances.size(); ++i) {
        const auto &draw = frame.instances[i][0].draw;
        for (; next < emitting.size(); ++next) {
          const auto &unit = all[emitting[next]];
          if (std::tie(draw.classification, draw.instanceId, draw.instanceIndex) <
              std::make_tuple(uint32_t(REMIXAPI_INSTANCE_STATIC_EXT), unit.id, unit.index))
            break;
          unitEmitters(unit);
        }
        const auto own = frame.flatEmitters(frame.emitterFirst[i], frame.emitterFirst[i + 1]);
        merged.insert(merged.end(), own.begin(), own.end());
      }
      for (; next < emitting.size(); ++next)
        unitEmitters(all[emitting[next]]);
      return merged;
    }
    return frame.flatEmitters();
  }

  TraceFrame prepareSceneFrame(NativeResources &resources, StaticScene *statics, OpacityCache &opacity,
                               WorkerPool *workers, bool opacityProfile, double seconds, bool filterEmission,
                               SurfaceEmissionCache &emissionCache) {
    using Clock = std::chrono::steady_clock;
    auto &costs = g_frameCosts;
    const bool timed = sceneTimings();
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto now = [&] { return timed ? Clock::now() : Clock::time_point{}; };
    const auto began = now();
    bool units = statics && !opacityProfile && statics->sync(resources, opacity, emissionCache, workers);
    const auto synced = now();
    costs.sync += ms(began, synced);
    if (units) {
      // Frame-local static instances would interleave with the set, and
      // colliding set keys make prepareTraceFrame reject the frame: trace
      // such a frame from the full draw list, exactly as without the set.
      bool local = statics->duplicates();
      for (const auto &draw : resources.draws())
        local |= draw.traced() && draw.classification == REMIXAPI_INSTANCE_STATIC_EXT;
      units = !local;
    }
    if (!units)
      resources.expandStatic();
    costs.local += ms(synced, now());
    auto frame = prepareTraceFrame(resources.draws(), seconds, filterEmission, emissionCache,
                                   units ? statics : nullptr);
    frame.staticUnits = units;
    // Coverage proof is CPU-only and can overlap the previous GPU submission.
    // Keep the result on this immutable frame, alongside its animated UVs.
    // Cached partitions are found on workers; the rest (new, revalidated or
    // replaced entries) go through partition() in draw order, as before. The
    // profile's lookup and hit counters need every lookup in partition().
    const auto found = now();
    if (!opacityProfile && workers) {
      constexpr size_t instancesPerTask = 512;
      const size_t count = frame.instances.size();
      workers->run((count + instancesPerTask - 1) / instancesPerTask, [&](size_t task) {
        for (size_t i = task * instancesPerTask; i < std::min(count, (task + 1) * instancesPerTask); ++i)
          for (auto &surface : frame.instances[i])
            surface.opacity = opacity.find(surface.draw, surface.uv);
      });
    }
    const auto partitioned = now();
    costs.find += ms(found, partitioned);
    for (auto instance : frame.instances)
      for (auto &surface : instance)
        if (!surface.opacity)
          surface.opacity = opacity.partition(surface.draw, surface.uv);
    costs.partition += ms(partitioned, now());
    ++costs.frames;
    return frame;
  }
} // namespace remix::scene
