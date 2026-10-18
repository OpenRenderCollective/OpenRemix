#pragma once
#include "forward/lighting.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace remix::forward::mixed {
  inline bool outsideEmissionCone(const scene::SphereLight &light, const Bounds &bounds) {
    if (!bounds.valid || !(light.coneCos > 0)) return false;
    double axisLength2 = 0, distance2 = 0, halfDiagonal2 = 0, projection = 0, coordinateScale = 0;
    for (uint32_t c = 0; c < 3; ++c) {
      if (bounds.minimum[c] > bounds.maximum[c]) return false;
      const double centre = (double(bounds.minimum[c]) + bounds.maximum[c]) * .5;
      const double halfSize = (double(bounds.maximum[c]) - bounds.minimum[c]) * .5;
      const double delta = centre - light.position[c];
      axisLength2 += double(light.direction[c]) * light.direction[c];
      projection += delta * light.direction[c];
      distance2 += delta * delta;
      halfDiagonal2 += halfSize * halfSize;
      coordinateScale += std::abs(centre) + std::abs(double(light.position[c]));
    }
    if (!(axisLength2 > 0) || !std::isfinite(axisLength2)) return false;
    const double axisLength = std::sqrt(axisLength2);
    // Match the GPU dot product even for an adapter supplying a nonunit axis.
    const double cosine = light.coneCos / axisLength;
    if (!(cosine > 0 && cosine < 1)) return false;
    double emitterRadius = light.radius;
    if (light.triangle) {
      emitterRadius = 0;
      for (const auto &corner : light.corners) {
        double radius2 = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          const double delta = double(corner[c]) - light.position[c];
          radius2 += delta * delta;
        }
        if (!std::isfinite(radius2)) return false;
        emitterRadius = std::max(emitterRadius, std::sqrt(radius2));
      }
    }
    if (!(emitterRadius >= 0) || !std::isfinite(emitterRadius)) return false;
    const double axial = projection / axisLength;
    const double radial = std::sqrt(std::max(0., distance2 - axial * axial));
    const double sine = std::sqrt(std::max(0., 1 - cosine * cosine));
    // Every emitter-to-receiver connection lies in this ball. The signed
    // cone-side expression is 1-Lipschitz, so a margin larger than the ball
    // radius proves that no connection can enter the emitting cone.
    const double radius = std::sqrt(halfDiagonal2) + emitterRadius;
    const double margin = 64 * std::numeric_limits<float>::epsilon() *
                          (coordinateScale + std::sqrt(distance2) + radius);
    return cosine * radial - sine * axial > radius + margin;
  }
} // namespace remix::forward::mixed
