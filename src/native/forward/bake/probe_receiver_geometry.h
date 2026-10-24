#pragma once
#include "scene/types.h"

namespace remix::forward::bake {
  // Geometry-only receivers cannot promise alpha-texel coverage. Keep picking
  // and model-support sampling on the same rigid opaque eligibility policy.
  inline bool opaqueProbeReceiver(const scene::Draw &draw) {
    if (!draw.surface || !draw.traced() || draw.decal || draw.viewModel || draw.reflectionOnly || draw.wireframe ||
        draw.bones || draw.blend.enabled ||
        (draw.material && (draw.material->transmission || draw.material->translucent))) return false;
    const uint32_t compare = draw.alphaOverride || (draw.material && draw.material->drawAlpha)
        ? draw.compare : draw.material ? draw.material->compare : 7;
    return draw.ignoreAlpha || compare == 7;
  }
}
