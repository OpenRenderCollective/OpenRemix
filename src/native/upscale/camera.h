#pragma once
#include "scene/types.h"
#include <cmath>

namespace remix::upscale {
  struct DepthRange {
    float nearPlane, farPlane;
  };
  inline DepthRange depthRange(const scene::Matrix &projection) {
    // Solve clip Z/W at 0 and 1 directly. Subtracting entries of an FP32
    // inverse loses the far plane for the very wide frusta used by USD captures.
    const double z = projection[10], w = projection[11], offset = projection[14];
    return {float(std::abs(offset / z)), float(std::abs(offset / (z - w)))};
  }
  inline scene::Matrix reversedDepth(const scene::Matrix &projection) {
    auto result = projection;
    // Convert the projection before evaluating depth. Subtracting a rounded
    // device-depth texture from one cannot recover its lost precision.
    for (unsigned column = 0; column < 4; ++column)
      result[column * 4 + 2] = projection[column * 4 + 3] - projection[column * 4 + 2];
    return result;
  }
} // namespace remix::upscale
