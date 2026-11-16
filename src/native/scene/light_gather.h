#pragma once
#include "scene/emission.h"
#include "scene/light_bvh.h"
#include "scene/static_scene.h"
#include "scene/trace_frame.h"
#include <memory>
#include <span>
#include <vector>

namespace remix::scene {
  // The light tree's input, gathered every frame: the sphere lights, then the
  // static set's emitters (StaticScene::emitting), then the frame's emitter
  // runs, each light once, leaving out those without size (and every emitter
  // when emissive lights are off). Kept from frame to frame with the sources
  // its parts came from: a part whose source is last frame's (the same build
  // of a static unit, the same immutable run) is not gathered again unless
  // its emission is filtered (EmissionHistory). When every part kept its place
  // and size the input keeps its layout, and the gather names the ranges of it
  // that changed (LightBvh::InputChanges): a level's emitters at rest are then
  // neither copied nor compared, by the gather or by the light tree.
  class LightGather {
  public:
    struct Settings {
      bool emissiveLights = true; // emitters at all
      bool filterEmission = false; // static units' textured emitters go through the history
      bool operator==(const Settings &) const = default;
    };
    // This frame's input (kept here until the next call). `changes` numbers
    // it and, when the layout is last frame's, names what changed.
    const std::vector<SphereLight> &gather(std::span<const SphereLight> spheres, const TraceFrame &frame,
                                           const StaticScene *statics, EmissionHistory &history,
                                           const Settings &settings, LightBvh::InputChanges &changes);
    const std::vector<SphereLight> &lights() const { return m_lights; }
    // The input's first lights are the sphere lights.
    size_t sphereCount() const { return m_segments.empty() ? 0 : m_segments[0].count; }
    // Lights gathered again last call (the rest were kept in place or copied).
    size_t regathered() const { return m_staging.size(); }

  private:
    // One part of the input: the spheres (no source), a static unit (its slot
    // + 1, its generation) or a run (its list, kept alive by m_held so that
    // its address names it).
    struct Segment {
      const void *source = nullptr;
      uint64_t token = 0;
      uint32_t first = 0, count = 0;
      bool animated = false, dynamic = false;
    };
    std::vector<Segment> m_segments, m_next;
    std::vector<std::shared_ptr<const std::vector<SphereLight>>> m_held, m_nextHeld;
    // Per next segment: where its lights start in m_staging, ~0u: kept.
    std::vector<uint32_t> m_fresh;
    std::vector<SphereLight> m_lights, m_spare, m_staging;
    Settings m_settings;
    uint64_t m_serial = 0;
    bool m_valid = false;
  };
} // namespace remix::scene
