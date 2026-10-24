#pragma once
#include "scene/types.h"
#include <stop_token>
#include <vector>

namespace remix::scene { struct BakeScene; }

namespace remix::forward::bake {
  struct ReceiverRange { uint32_t draw = 0, firstIndex = 0, indexCount = 0; };
  // Disjoint indexed ranges not already owned by caller-authored charts.
  std::vector<ReceiverRange> unchartedReceiverRanges(const scene::BakeScene &source,
                                                    std::stop_token cancelled = {});
  // Conservative BSP exclusion: reject a triangle only when no fragment reaches
  // a playable leaf. Invalid/over-budget partitions keep coverage, never guess.
  bool playableReceiverTriangle(const scene::BakeScene &source,
      const std::array<std::array<float, 3>, 3> &positions, std::stop_token cancelled = {});
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
