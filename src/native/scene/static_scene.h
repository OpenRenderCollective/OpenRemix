#pragma once
#include "scene/emission.h"
#include "scene/opacity.h"
#include "scene/temporal.h"
#include "scene/trace_frame.h"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace remix {
  class NativeResources;
  class WorkerPool;
} // namespace remix

namespace remix::scene {
  // One traced instance of the persistent static set: a retained handle's GPU
  // instance, with its surfaces prepared exactly as prepareTraceFrame would
  // prepare its draws (decal state, UVs, opacity partitions). Units live in
  // stable slots; a slot is reused only after the tracer acknowledged its
  // removal.
  struct StaticUnit {
    uint64_t handle = 0, id = 0;
    uint32_t index = 0;         // instanceIndex
    uint64_t generation = 0;    // changes whenever the unit is (re)built
    bool live = false;          // the slot holds a unit
    bool singular = false;      // dropped from tracing, as prepareTraceFrame drops it
    bool pending = false;       // listed in rebuilt()
    uint32_t pbrSurfaces = 0;   // surfaces with a PBR material
    std::vector<TraceFrame::Surface> surfaces; // mesh surface order
    // Its emissive triangles, as prepareTraceFrame emits them; animated says
    // whether the surface has an emission texture (animated when filtering).
    std::vector<TraceFrame::Emitter> emitters;
  };

  // What one unit was in one prepared frame, for temporal history: the draws
  // TemporalHistory would have staged and the vertex buffers the tracer would
  // have kept as previous vertices.
  struct StaticUnitState {
    std::vector<TemporalHistory::SurfaceHistory> surfaces; // by surface index
    std::vector<std::shared_ptr<const void>> owners;       // keep the vertex buffers alive
    std::vector<uint64_t> vertices;                        // their addresses, by surface index
  };

  // The tracer's copy of the persistent static set (NativeResources::
  // setPersistentStatic), updated from the handles that changed. Also the
  // temporal history of the set: units are not staged into TemporalHistory
  // every frame; instead each unit's state is recorded when it changes, and
  // looked up for the last committed frame.
  class StaticScene : public TemporalHistory::Fallback {
  public:
    struct Statistics {
      uint64_t units = 0, surfaces = 0, rebuiltUnits = 0, removedUnits = 0, changes = 0, resyncs = 0;
      double milliseconds = 0; // sync()
    };
    // Brings the units up to date with resources' static set. False when this
    // frame's set is not drawn through the table; the units are then unused.
    bool sync(NativeResources &resources, OpacityCache &opacity, SurfaceEmissionCache &emission,
              WorkerPool *workers);
    const std::vector<StaticUnit> &units() const { return m_units; }
    // Live units in trace order ((instanceId, instanceIndex) ascending).
    const std::vector<uint32_t> &order() const { return m_order; }
    // Units with emitters, in trace order.
    const std::vector<uint32_t> &emitting() const { return m_emitting; }
    // Since the last acknowledge(): units built or rebuilt, units removed
    // (their slots and keys), and whether order() changed.
    const std::vector<uint32_t> &rebuilt() const { return m_rebuilt; }
    struct Removed {
      uint32_t slot;
      uint64_t id;
      uint32_t index;
    };
    const std::vector<Removed> &removed() const { return m_removed; }
    bool orderChanged() const { return m_orderChanged; }
    void acknowledge();
    // Keys of live units, for prepareTraceFrame's identity rules.
    bool hasId(uint64_t id) const { return m_ids.count(id) != 0; }
    bool contains(uint64_t id, uint32_t index) const { return m_keys.count({id, index}) != 0; }
    bool duplicates() const { return m_duplicates != 0; }
    // Live, non-singular units and their surfaces.
    size_t tracedUnits() const { return m_tracedUnits; }
    size_t tracedSurfaces() const { return m_tracedSurfaces; }
    // Surfaces of all live units (singular ones included, as the frame's
    // draw list includes them), with and without a PBR material.
    size_t liveSurfaces() const { return m_liveSurfaces; }
    size_t livePbrSurfaces() const { return m_livePbr; }
    // Forget everything (tracer reset); the next sync rebuilds all units.
    void clear();
    // The next sync rebuilds every unit, as for a new set, keeping history:
    // their opacity partitions depend on a changed OpacityCache setting.
    void invalidate() { m_epoch = ~0ull; }
    const Statistics &statistics() const { return m_statistics; }
    void resetStatistics() { m_statistics = {}; }

    // History. serial numbers prepared frames. record(): the unit (id, index)
    // was traced with state in frame serial (null: not traced). absent(): no
    // unit took part in frame serial. committed(): frame serial became the
    // committed history; states no lookup can reach any more go to retired.
    void record(uint64_t id, uint32_t index, uint64_t serial, std::shared_ptr<const StaticUnitState> state);
    void absent(uint64_t serial);
    void committed(uint64_t serial, std::vector<std::shared_ptr<const void>> &retired);
    // The unit's state in the committed frame, or null.
    const StaticUnitState *committedState(uint64_t id, uint32_t index) const;
    const TemporalHistory::SurfaceHistory *previous(const TemporalKey &key) const override;
    // Previous vertex buffer address of key, or 0.
    uint64_t previousVertices(const TemporalKey &key) const;
    bool hasCommitted() const { return m_hasCommitted; }
    uint64_t committedSerial() const { return m_committed; }
    // The committed frame was traced without the set (absent()).
    bool committedAbsent() const;

  private:
    static uint64_t key(uint64_t id, uint32_t index) {
      uint64_t h = id ^ (uint64_t(index) + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
      return h ^ (h >> 31);
    }
    struct KeyHash {
      size_t operator()(const std::pair<uint64_t, uint32_t> &k) const { return size_t(key(k.first, k.second)); }
    };
    void build(const struct RetainedInstance &entry, SurfaceEmissionCache &emission,
               const class NativeResources *resources = nullptr);
    void remove(uint64_t handle);
    void removeUnit(uint32_t slot);
    uint32_t allocate();
    void sort();
    void prune(std::pair<uint64_t, uint32_t> key, std::vector<std::shared_ptr<const void>> *retired);

    std::vector<StaticUnit> m_units;
    std::vector<uint32_t> m_free, m_freeing, m_order, m_rebuilt, m_emitting;
    std::vector<Removed> m_removed;
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_handles; // handle -> units by index
    std::unordered_map<std::pair<uint64_t, uint32_t>, uint32_t, KeyHash> m_keys; // (id, index) -> unit count
    std::unordered_map<uint64_t, uint32_t> m_ids;                  // id -> unit count
    uint64_t m_epoch = ~0ull, m_generation = 0;
    uint32_t m_duplicates = 0;
    size_t m_tracedUnits = 0, m_tracedSurfaces = 0, m_liveSurfaces = 0, m_livePbr = 0;
    bool m_orderChanged = false, m_orderDirty = false, m_emittingDirty = false;
    std::vector<uint64_t> m_changes;
    std::vector<uint32_t> m_partitionPending;
    Statistics m_statistics;

    using Timeline = std::vector<std::pair<uint64_t, std::shared_ptr<const StaticUnitState>>>;
    std::unordered_map<std::pair<uint64_t, uint32_t>, Timeline, KeyHash> m_timelines;
    std::vector<std::pair<uint64_t, uint32_t>> m_prunable; // timelines with more than one state, or ending unused
    std::vector<uint64_t> m_absent;                        // ascending serials
    uint64_t m_committed = 0;
    bool m_hasCommitted = false;
  };

  // The CPU half of PathTracer::prepareScene. With statics, the static set
  // drawn this frame (NativeResources::staticDrawn) stays in statics and
  // frame.staticUnits is set; otherwise, and whenever the frame could not be
  // traced exactly that way (frame-local static instances, the opacity
  // profile), the set is expanded into the draw list and traced as before.
  TraceFrame prepareSceneFrame(NativeResources &resources, StaticScene *statics, OpacityCache &opacity,
                               WorkerPool *workers, bool opacityProfile, double seconds, bool filterEmission,
                               SurfaceEmissionCache &emissionCache);
  // A frame's emitters in trace order: with frame.staticUnits, the set's
  // (StaticScene::emitting) merged with the frame's own, which are all that
  // frame.emitters holds; otherwise frame.emitters. PathTracer::prepareScene
  // reads the two lists where they are (the light tree orders its input):
  // merging copied every static emitter each frame.
  std::vector<TraceFrame::Emitter> mergedEmitters(const TraceFrame &frame, const StaticScene *statics,
                                                  bool filterEmission);
} // namespace remix::scene
