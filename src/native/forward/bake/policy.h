#pragma once
#include "forward/bake/data.h"
#include <span>

namespace remix::forward::bake {
  enum class DirectMode : uint8_t { Mixed, Baked, Dynamic };
  struct DirectPolicy {
    // Captured light indices are stable across transport and runtime bindings.
    // Empty retains the legacy per-light path while integration is in progress.
    std::vector<DirectMode> lights;
  };
  // Explicit membership first; automatic selection is a separate planning step.
  // Moving lights cannot contribute to immutable diffuse lightmaps.
  DirectPolicy directPolicy(const scene::BakeScene &source, std::span<const uint32_t> bakedLights);
  bool validateDirectPolicy(const scene::BakeScene &source, const DirectPolicy &policy);
  // Validate the complete policy before changing any packed input. Returns true
  // when aggregate direct sampling is needed; never reorders captured lights.
  bool applyDirectPolicy(const scene::BakeScene &source, const DirectPolicy &policy,
                         std::span<LightData> lights);
} // namespace remix::forward::bake
