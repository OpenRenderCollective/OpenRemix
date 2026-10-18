#pragma once
#include "forward/bake/data.h"
#include <span>

namespace remix::forward::bake {
  // BakedMixed owns diffuse in the aggregate while retaining attributed data
  // for specular visibility and moving-caster corrections. Baked needs neither.
  // Append new values: these bytes are part of the private bake cache format.
  enum class DirectMode : uint8_t { Mixed, Baked, Dynamic, BakedMixed };
  constexpr bool aggregatesDiffuse(DirectMode mode) {
    return mode == DirectMode::Baked || mode == DirectMode::BakedMixed;
  }
  constexpr bool retainsVisibility(DirectMode mode) {
    return mode == DirectMode::Mixed || mode == DirectMode::BakedMixed;
  }
  struct DirectPolicy {
    // Captured light indices are stable across transport and runtime bindings.
    // Empty retains the legacy per-light path while integration is in progress.
    std::vector<DirectMode> lights;
  };
  // Explicit membership first; automatic selection is a separate planning step.
  // Moving lights cannot contribute to immutable diffuse lightmaps.
  DirectPolicy directPolicy(const scene::BakeScene &source, std::span<const uint32_t> bakedLights,
                            DirectMode selectedMode = DirectMode::Baked);
  // Permanent aggregate RGB excludes an entire shared identity if any emitter
  // is moving or styled. The selected mode controls retained mixed visibility.
  DirectPolicy staticAggregatePolicy(const scene::BakeScene &source,
                                     DirectMode mode = DirectMode::BakedMixed,
                                     bool bakeGeometryLights = false);
  bool validateDirectPolicy(const scene::BakeScene &source, const DirectPolicy &policy);
  // Validate the complete policy before changing any packed input. Returns true
  // when aggregate direct sampling is needed; never reorders captured lights.
  bool applyDirectPolicy(const scene::BakeScene &source, const DirectPolicy &policy,
                         std::span<TransportLight> lights);
} // namespace remix::forward::bake
