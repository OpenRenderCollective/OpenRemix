#pragma once
#include "forward/bindings.h"

namespace remix::forward {
  constexpr uint32_t skipUnitDirect = 16; // DrawData::maps.x; mirrors forward.frag
  Matrix normalMatrix(const Matrix &model);
  DrawData materialData(const scene::Draw &draw, double seconds = 0);
} // namespace remix::forward
