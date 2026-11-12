#pragma once
#include "forward/bindings.h"

namespace remix::forward {
  constexpr uint32_t skipUnitDirect = 16; // DrawData::maps.x; mirrors forward.frag
  constexpr uint32_t reflectionLayerShift = 8; // one-based active layer in DrawData::maps.x bits 8..14
  // Conservative draw-wide proof; texture/mask-dependent diffuse stays generic.
  inline uint32_t alphaComparison(const scene::Draw &draw) {
    return draw.ignoreAlpha ? 7u : draw.alphaOverride ? draw.compare : draw.material->compare;
  }
  Matrix normalMatrix(const Matrix &model);
  // normalMatrix for a frame's draws and bones: a collapsed (singular) transform, such as
  // a bone scaled to zero to hide a limb, maps its geometry to nothing and has no normal
  // transform; it gets a zero matrix instead of failing the frame.
  Matrix drawNormalMatrix(const Matrix &model);
  DrawData materialData(const scene::Draw &draw, double seconds = 0);
} // namespace remix::forward
