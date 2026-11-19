#include "scene/light_gather.h"
#include <algorithm>

namespace remix::scene {
  const std::vector<SphereLight> &LightGather::gather(std::span<const SphereLight> spheres, const TraceFrame &frame,
                                                      const StaticScene *statics, EmissionHistory &history,
                                                      const Settings &settings, LightBvh::InputChanges &changes) {
    changes.serial = ++m_serial;
    changes.previous = 0;
    changes.ranges.clear();
    const bool sameSettings = m_valid && settings == m_settings;
    m_next.clear();
    m_nextHeld.clear();
    m_fresh.clear();
    m_staging.clear();
    // Last frame's part at this place, when it is this one's source unchanged.
    const auto kept = [&](const Segment &segment) -> const Segment * {
      const size_t index = m_next.size();
      if (!sameSettings || index >= m_segments.size())
        return nullptr;
      const auto &old = m_segments[index];
      return old.source == segment.source && old.token == segment.token && old.dynamic == segment.dynamic ? &old
                                                                                                           : nullptr;
    };
    const auto add = [&](Segment segment, uint32_t fresh) {
      if (fresh != ~0u)
        segment.count = uint32_t(m_staging.size() - fresh);
      m_next.push_back(segment);
      m_fresh.push_back(fresh);
    };
    // The spheres, every frame (a game's lights move and blink).
    m_staging.insert(m_staging.end(), spheres.begin(), spheres.end());
    add({}, 0);
    if (settings.emissiveLights) {
      const auto take = [&](SphereLight light, bool animated, uint64_t layout) {
        if (animated)
          history.apply(light, layout);
        if (light.radius > 1e-8f)
          m_staging.push_back(light);
      };
      if (statics)
        for (const uint32_t slot : statics->emitting()) {
          const auto &unit = statics->units()[slot];
          Segment segment{reinterpret_cast<const void *>(uintptr_t(slot) + 1), unit.generation};
          const auto *old = kept(segment);
          if (old)
            segment.animated = old->animated;
          else
            for (const auto &emitter : unit.emitters)
              segment.animated |= settings.filterEmission && emitter.animated;
          // Filtered emission changes every frame: gathered again.
          if (old && !segment.animated) {
            segment.count = old->count;
            add(segment, ~0u);
            continue;
          }
          const auto fresh = uint32_t(m_staging.size());
          for (const auto &emitter : unit.emitters)
            take(emitter.light, settings.filterEmission && emitter.animated, emitter.layout);
          add(segment, fresh);
        }
      for (const auto &run : frame.emitters) {
        Segment segment{run.lights.get(), 0, 0, 0, run.animated, run.dynamic};
        m_nextHeld.push_back(run.lights);
        if (const auto *old = kept(segment); old && !run.animated) {
          segment.count = old->count;
          add(segment, ~0u);
          continue;
        }
        const auto fresh = uint32_t(m_staging.size());
        const auto &source = *run.lights;
        for (size_t i = 0; i < source.size(); ++i) {
          auto light = source[i];
          light.dynamic = run.dynamic;
          take(light, run.animated, run.animated ? (*run.reduction)[i].layout : 0);
        }
        add(segment, fresh);
      }
    }

    bool sameLayout = sameSettings && m_next.size() == m_segments.size();
    for (size_t s = 0; sameLayout && s < m_next.size(); ++s)
      sameLayout = m_next[s].count == m_segments[s].count;
    if (sameLayout) {
      // In place: the parts gathered again are compared with what they replace.
      for (size_t s = 0; s < m_next.size(); ++s) {
        auto &segment = m_next[s];
        segment.first = m_segments[s].first;
        if (m_fresh[s] == ~0u)
          continue;
        for (uint32_t k = 0; k < segment.count; ++k) {
          const auto &light = m_staging[m_fresh[s] + k];
          auto &held = m_lights[segment.first + k];
          if (held == light)
            continue;
          held = light;
          const uint32_t index = segment.first + k;
          if (!changes.ranges.empty() && changes.ranges.back().second == index)
            ++changes.ranges.back().second;
          else
            changes.ranges.emplace_back(index, index + 1);
        }
      }
      changes.previous = m_serial - 1;
    } else {
      // A new layout: kept parts are copied from last frame's input.
      m_spare.clear();
      for (size_t s = 0; s < m_next.size(); ++s) {
        auto &segment = m_next[s];
        const auto first = uint32_t(m_spare.size());
        if (m_fresh[s] == ~0u) {
          const auto *from = m_lights.data() + m_segments[s].first;
          m_spare.insert(m_spare.end(), from, from + segment.count);
        } else {
          const auto *from = m_staging.data() + m_fresh[s];
          m_spare.insert(m_spare.end(), from, from + segment.count);
        }
        segment.first = first;
      }
      m_lights.swap(m_spare);
    }
    m_segments.swap(m_next);
    m_held.swap(m_nextHeld);
    m_settings = settings;
    m_valid = true;
    return m_lights;
  }
} // namespace remix::scene
