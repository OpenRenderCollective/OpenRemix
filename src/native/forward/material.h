#pragma once
#include "forward/bindings.h"

namespace remix::forward {
  Matrix normalMatrix(const Matrix &model);
  DrawData materialData(const scene::Draw &draw, double seconds = 0);
} // namespace remix::forward
