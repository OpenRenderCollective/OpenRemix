#pragma once
#include <cstdlib>
#include "scene/types.h"
#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace remix::scene {
  // std430 records shared with light_bvh.glsl. Analytic emitters need no
  // geometry.
  struct LightEmitter {
    float positionRadius[4] = {};
    float radiance[4] = {};
    float corners[3][4] = {};
    float directionCone[4] = {}, shaping[4] = {};
  };
  struct LightNode {
    float minimumPower[4] = {};
    float maximum[4] = {}; // xyz bounds; w largest emitter radius squared
    // Internal: left, right, first leaf rank, leaf count. Leaf: leaf rank,
    // emitter count, first emitter, 1. A leaf's emitters are consecutive;
    // LightEmitter::shaping[2] holds the running share of the leaf's power
    // up to and including each, shaping[3] its own share (the shader picks
    // one by it: light_bvh.glsl leafEmitter).
    uint32_t links[4] = {};
  };
  static_assert(sizeof(LightEmitter) == 112 && sizeof(LightNode) == 48);
  // Dev aid (lighttree.request, api/lifecycle.cpp): the renderer saves its
  // next combined light tree to this path (LightBvh::save) and clears it.
  inline std::mutex g_lightTreeSaveMutex;
  inline std::string g_lightTreeSavePath;
  // A sphere light that moved, reshaped, changed power or switched on/off since
  // the previous update: its emitter now and before (zero radiance when absent).
  struct MovedLight {
    LightEmitter current, previous;
  };
  // The hierarchy has two parts. The static part holds the lights that were
  // present and at rest when it was built; its emitter and node indices stay
  // put until the next build (a light that leaves keeps its slot, unlit, and
  // returns to it). Lights that appear afterwards, and the emitters of
  // instances the host declared dynamic, form the dynamic part: a subtree of
  // its own behind the static nodes and emitters, rebuilt whenever its
  // membership changes. Only a build changes static indices, so only a build
  // invalidates what refers to them (visibility table, guiding history).
  // Without dynamic lights the hierarchy is the static part alone. Leaves are
  // runs of at most clusterSize() emitters of one surface draw
  // (SphereLight::group), in their spatial order; sphere lights and other
  // groups of one are leaves of their own. A build
  // keeps the slots of lights that were present within kRetain updates, so
  // lights that leave the host's view for a while (out of its culling) return
  // to their slots instead of to the dynamic part; a rebuild of the dynamic
  // part keeps those absent under kRebuildRetain updates.
  // REMIX_SCENE_TIMINGS=1: time scene preparation's steps (LightChurn::phases,
  // FrameCosts) for the timings dump; off, those clocks are not read.
  inline bool sceneTimings() {
    static const bool enabled = [] {
      const char *value = std::getenv("REMIX_SCENE_TIMINGS");
      return value && value[0] && value[0] != '0';
    }();
    return enabled;
  }

  // Dynamic part rebuilds of all trees since this was last read
  // (diagnostics): lights that appeared (and their surfaces) and slots of
  // lights gone that were dropped.
  struct LightChurn {
    uint64_t rebuilds = 0, added = 0, removed = 0, addedSurfaces = 0;
    uint32_t unlit = 0; // the last update's unlit dynamic slots
    // Time by phase of update (ms, REMIX_SCENE_TIMINGS): order, copy, moved+compare, static
    // match, dynamic merge, slot writes, bounds, builds, counts, significance.
    double phases[10] = {};
    uint64_t updates = 0, snapshots = 0; // significance snapshots taken
  };
  inline LightChurn g_lightChurn;

  class LightBvh {
  public:
    LightBvh() : m_instance(nextInstance()) {}
    // What the caller knows changed in an update's input (PathTracer's
    // gather): the input has the size of the one numbered `previous` and,
    // outside `ranges` ([first, end) input indices, ascending), the same lights
    // in the same places. Then only those lights are compared, written and
    // refitted, instead of all of them (most of a level's emitters are at rest
    // while a few lights move). A hint relative to another input than this
    // hierarchy's last one is ignored, as is one whose changed lights differ
    // in identity, type, surface, rank or dynamic flag, and one due a rebuild:
    // those updates compare everything. Either way the result is the same.
    struct InputChanges {
      uint64_t serial = 0, previous = 0; // this input's number, and the one it is relative to
      std::vector<std::pair<uint32_t, uint32_t>> ranges;
    };
    bool update(std::span<const SphereLight> lights, const InputChanges *changes);
    bool update(const std::vector<SphereLight> &lights, const InputChanges *changes = nullptr) {
      return update(std::span<const SphereLight>(lights), changes);
    }
    // Upload deltas. A GPU copy made at `generation` of this hierarchy (by
    // instance()) is brought up to date by copying only the emitters and
    // nodes changed since, as [first, end) ranges (nearby ones merged):
    // refits mark what bounds() rewrote. False when it must be copied whole
    // (built since, or another hierarchy's copy). Props flickering in and out
    // of view changed a few thousand of ~100k emitters a frame while all
    // 12 MB were copied, ~4 ms over a Thunderbolt eGPU's PCIe link.
    using Ranges = std::vector<std::pair<uint32_t, uint32_t>>;
    bool changedSince(uint64_t instance, uint64_t generation, Ranges &emitters, Ranges &nodes) const;
    uint64_t instance() const { return m_instance; }
    // Emitters a leaf holds at most (1: a leaf per emitter). A change makes
    // the next update build the hierarchy anew.
    void setClusterSize(uint32_t size);
    uint32_t clusterSize() const { return m_clusterSize; }
    // Selection weights are power to this exponent (1: power). A change
    // makes the next update build anew.
    void setPowerExponent(float exponent);
    // The leaves of the build before the last one, by node index, to the
    // leaf now holding their first light (new index + 1; 0: gone), when the
    // last build followed `builds` == fromBuilds: what refers to leaves (the
    // light guide) carries over the rebuild. Null otherwise.
    const std::vector<uint32_t> *leafRemap(uint64_t fromBuilds) const {
      return fromBuilds + 1 == builds && !m_leafRemap.empty() ? &m_leafRemap : nullptr;
    }
    uint32_t leafCount() const { return uint32_t(m_clusters.size()); }
    const std::vector<LightEmitter> &emitters() const { return m_emitters; }
    const std::vector<LightNode> &nodes() const { return m_nodes; }
    // The node and emitter counts (two uint32), then the nodes and emitters
    // as uploaded, for offline analysis of selection probabilities.
    bool save(const char *path) const;
    // The lights in emitter order: static slots (by type and identity, as
    // built; unlit when the light is gone), then the dynamic lights.
    const std::vector<SphereLight> &lights() const { return m_lights; }
    // Lights that changed in the last update (at most kMaxMoved; the denoiser's
    // local history confidence). Empty when nothing changed.
    static constexpr size_t kMaxMoved = 16;
    const std::vector<MovedLight> &moved() const { return m_moved; }
    // builds: the whole hierarchy (static indices changed). refits: updates
    // that kept them. dynamicBuilds: refits that rebuilt the dynamic part.
    uint64_t generation = 0, builds = 0, refits = 0, dynamicBuilds = 0;
    uint64_t changedOnly = 0; // updates that read only the changes they were told of (InputChanges)
    uint64_t sorts = 0; // updates whose input came in a new order
    enum Reason : uint32_t { None, Initial, FirstDynamic, Settled, Retired, Cleared, ReasonCount };
    uint64_t buildReasons[ReasonCount] = {};
    uint32_t lastBuildReason = None;
    // Static slots in use, dynamic lights present, static slots whose light is gone,
    // and lights of host-declared dynamic instances.
    uint32_t staticLights = 0, dynamicLights = 0, retiredSlots = 0, instanceLights = 0;
    uint32_t unlitDynamicSlots = 0; // dynamic slots whose light is gone (kept kRetain updates)
    // Emitters [0, staticEmitters()) and nodes [0, staticNodes()) belong to
    // the static part (with the root and the dynamic part's root).
    uint32_t staticEmitters() const { return m_staticCount; }
    uint32_t staticNodes() const { return m_staticNodes; }
    // Separate transport updates, scene-wide temporal response and the spatial
    // correspondence used by visibility guidance. Color animation changes only
    // transport unless its aggregate power change is significant.
    uint64_t lightingGeneration = 0, visibilityGeneration = 0;
    uint32_t active = 0, depth = 0;
    double updateMilliseconds = 0;
    // The last build (builds), and all of them.
    double buildMilliseconds = 0, buildTotalMilliseconds = 0;
    // Same bounded-distance importance estimate as the shader; useful for PDF
    // validation.
    static double weight(const LightNode &node, const float position[3], const float normal[3], double range = 0);
    static double falloff(double distanceSquared, double range);
    // Updates a new light stays in the dynamic part before it counts as at
    // rest, and between builds.
    static constexpr uint64_t kSettle = 60, kBuildInterval = 120;
    // Updates an absent light keeps its static slot through builds, and its
    // dynamic one while the dynamic part's lights stay the same.
    static constexpr uint64_t kRetain = 1800;
    // Updates an absent light keeps its dynamic slot through a rebuild of
    // the dynamic part (one another light's arrival forces).
    static constexpr uint64_t kRebuildRetain = 8;

  private:
    std::vector<MovedLight> m_moved;
    // staticSeen: the update each static light was last present in (absent
    // ones keep their slot, unlit).
    void build(std::vector<SphereLight> statics, std::vector<uint64_t> staticStamps, std::vector<uint64_t> staticSeen,
               std::vector<SphereLight> dynamics, std::vector<uint64_t> dynamicStamps,
               std::vector<uint64_t> dynamicSeen, Reason reason);
    void buildDynamic();
    // Leaves over the emitters [begin, end) (m_clusters, m_centers).
    void makeClusters(uint32_t begin, uint32_t end);
    // A subtree over the leaves order[begin, end).
    void buildInto(uint32_t index, std::vector<uint32_t> &order, uint32_t begin, uint32_t end, uint32_t level);
    void bounds(uint32_t index);
    void significance(const std::vector<SphereLight> &sorted);
    // An update of only the input ranges `changes` names (update's
    // InputChanges): false, having changed nothing, when it does not apply
    // and everything must be compared. `result` is update's.
    bool updateChanged(std::span<const SphereLight> input, const InputChanges &changes, bool &result);
    // After an update that compared everything: which slot holds each sorted
    // input light (m_slotOf), which dynamic slots have one, and the absent
    // static slots' retirement.
    void mapSlots();
    uint64_t due(uint64_t oldest, uint64_t newest, uint32_t stale) const;
    // m_order by (type, identity) of the input; radix sort on these.
    void sortOrder(std::span<const SphereLight> lights);
    std::vector<std::pair<uint64_t, uint32_t>> m_sortKeys, m_sortSpare;
    struct SortRun {
      uint32_t triangle;
      uint64_t group, id; // id: of a light without a group (each its own run)
      uint32_t first, count;
    };
    std::vector<SortRun> m_runs;
    // Per leaf: first emitter and count, and the center of its emitters.
    std::vector<std::pair<uint32_t, uint32_t>> m_clusters;
    std::vector<std::array<float, 3>> m_centers;
    uint32_t m_clusterSize = 1, m_staticClusters = 0;
    float m_powerExponent = 1;
    std::vector<uint32_t> m_leafRemap;
    // Input identity order survives radiance animation.
    std::vector<std::pair<uint64_t, uint32_t>> m_sourceKeys;
    std::vector<uint32_t> m_order, m_parents, m_leaves;
    std::vector<uint8_t> m_dirty;
    std::vector<SphereLight> m_sorted; // the last input, by type and identity
    std::vector<SphereLight> m_input;  // this update's input in that order (storage kept)
    std::vector<SphereLight> m_lights; // per emitter
    std::vector<SphereLight> m_temporalLights;
    std::vector<double> m_temporalEnergies, m_energyScratch; // per snapshot light; scratch
    std::vector<LightEmitter> m_emitters;
    std::vector<LightNode> m_nodes;
    // Per emitter: the update its geometry last changed in (or it appeared).
    std::vector<uint64_t> m_stamps;
    std::vector<uint8_t> m_present; // per static slot: its light is submitted
    std::vector<uint64_t> m_seen;   // per static slot: the update it was last submitted in
    // Per emitter and node: the generation that last rewrote it (refits);
    // m_structure: the generation of the last build (all rewritten).
    std::vector<uint64_t> m_emitterChanged, m_nodeChanged;
    uint64_t m_structure = 0, m_changing = 0; // m_changing: this update's generation
    uint64_t m_instance = 0;
    static uint64_t nextInstance() {
      static std::atomic<uint64_t> next{1};
      return next++;
    }
    std::vector<uint64_t> m_dynamicSeen; // the same per dynamic light (absent ones are kept unlit)
    // For updates of only what changed (updateChanged), kept by the updates
    // that compare everything: the input this hierarchy last took (its
    // InputChanges::serial, 0: unnumbered), each input light's place in the
    // sorted input (m_order inverted), each sorted light's slot, which dynamic
    // slots have a light, the static slots gone long enough to drop at the next
    // build and the update the next of the others is.
    uint64_t m_inputSerial = 0;
    // The last update that matched the input to the slots (each present
    // static slot was seen then): what a slot that leaves records as its last
    // sighting, as one stamped by every such update would.
    uint64_t m_lastMatched = 0;
    std::vector<uint32_t> m_position, m_slotOf;
    std::vector<uint8_t> m_dynamicPresent;
    uint32_t m_retired = 0, m_stale = 0;
    uint64_t m_nextStale = ~0ull;
    // significance's comparison with its snapshot, per sorted light (its
    // energy, its share of the change and its snapshot light, ~0u: none) and
    // in sum (the change, and the snapshot's and the input's energy): an
    // update of a few lights moves the sums by theirs. Valid while sorted
    // positions are those significance last saw.
    std::vector<double> m_lightEnergies, m_lightChanges;
    std::vector<uint32_t> m_snapshotOf;
    double m_changedEnergy = 0, m_snapshotEnergy = 0, m_inputEnergy = 0;
    bool m_significanceValid = false;
    // Scratch of updateChanged (storage kept): changed input index and
    // sorted position, the lights they replaced, nodes to refit.
    std::vector<std::pair<uint32_t, uint32_t>> m_changed;
    std::vector<SphereLight> m_replaced;
    std::vector<uint32_t> m_refit;
    std::vector<uint8_t> m_refitMarks; // per node, all zero between updates
    // Scratch of update (storage kept): see there.
    std::vector<uint32_t> m_extraIndex, m_mergeSource;
    std::vector<uint64_t> m_mergeStamps, m_mergeSeen;
    bool m_split = false; // root -> static part, dynamic part
    // Static and dynamic emitters; a part without lights keeps one unlit slot.
    uint32_t m_staticReal = 0, m_dynamicReal = 0;
    uint32_t m_staticCount = 0, m_staticNodes = 0, m_staticDepth = 0;
    uint32_t m_splitDepthLimit = 0; // buildInto: cost-chosen splits above this level
    uint64_t m_update = kSettle, m_builtAt = 0, m_due = ~0ull;
  };
} // namespace remix::scene
