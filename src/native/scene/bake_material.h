#pragma once
#include "scene/types.h"
#include <algorithm>
#include <cmath>

namespace remix::scene {
  // A stable homogeneous body with numerically bounded optical parameters.
  // Kept CPU-only and shared by material creation, capture and ray packing.
  inline bool validSmoothBoundary(const Material &material) {
    return material.transmission && std::isfinite(material.refractiveIndex) &&
           material.refractiveIndex >= 1 && material.refractiveIndex <= 4 &&
           std::isfinite(material.transmittanceDistance) && material.transmittanceDistance >= 1e-6f &&
           std::all_of(material.tint, material.tint + 3,
                       [](float color) { return std::isfinite(color) && color >= 0 && color <= 1; });
  }
} // namespace remix::scene
