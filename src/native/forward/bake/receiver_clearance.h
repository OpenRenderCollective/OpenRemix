#pragma once
#include "forward/lighting.h"
#include "scene/bake_scene.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace remix::forward::bake {
  // Stop a normal offset before its first solid interval in a closed host
  // partition. Unknown or already-solid starts have no admissible clearance.
  inline std::optional<float> partitionContainedBias(const scene::BakeScene &source,
      Vec3 position, Vec3 normal, float bias) {
    if (!source.partition || !source.partition->closed || !(source.sceneScale > 0) ||
        !std::isfinite(source.sceneScale) || !std::isfinite(bias) || bias < 0) return {};
    const float epsilon = .0001f / source.sceneScale, initial = std::min(epsilon, bias);
    Vec3 start, end;
    for (uint32_t c = 0; c < 3; ++c) {
      start[c] = position[c] + normal[c] * initial;
      end[c] = position[c] + normal[c] * bias;
    }
    const auto interval = source.partition->firstSolid(start.data(), end.data());
    using State = scene::VisibilityPartition::SegmentState;
    if (interval.state == State::Clear) return bias;
    if (interval.state != State::Solid || interval.fraction <= 0) return {};
    return std::max(0.f, float(initial + (bias - initial) * interval.fraction) - epsilon);
  }
}
