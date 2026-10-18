#pragma once
#include "forward/bake/data.h"
#include "scene/light_bvh.h"
#include <span>

namespace remix::forward::bake {
  // Reuse the scene light hierarchy while retaining captured light indices.
  // Mixed-light ownership and explicit visibility queries keep their ordering.
  std::vector<scene::LightNode> lightSelectionTree(std::span<const LightData> lights,
                                                 bool includeStyled = false);
} // namespace remix::forward::bake
