#pragma once
#include "scene/projection_space.h"
#include <vector>
namespace remix::scene {
  struct ReflectionVolume {
    uint64_t id = 0;
    std::array<float, 3> minimum{}, maximum{}, capture{};
    ProjectionSpace worldToLocal = worldSpace;
    int32_t priority = 0;
    bool operator==(const ReflectionVolume &) const = default;
  };
  // Immutable authoring state, separate from frozen diffuse lighting ownership.
  struct ReflectionVolumes {
    uint64_t sceneId = 0, revision = 0;
    std::vector<ReflectionVolume> volumes;
  };
} // namespace remix::scene
