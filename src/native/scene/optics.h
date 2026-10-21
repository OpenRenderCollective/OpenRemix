#pragma once
#include "scene/bake_material.h"
#include <array>
#include <optional>
#include <stdexcept>

namespace remix::scene {
  // Homogeneous optical properties in scene units. This is shared material
  // data; choosing a live ripple normal or a frozen mean interface is a pass
  // policy, not a different absorption model.
  struct OpticalMedium {
    std::array<float, 3> extinction{};
    float refractiveIndex = 1;

    float normalReflectance() const {
      const float ratio = (refractiveIndex - 1) / (refractiveIndex + 1);
      return ratio * ratio;
    }
    std::array<float, 3> transmittance(float distance) const {
      if (!std::isfinite(distance))
        throw std::invalid_argument("Nonfinite optical path length");
      std::array<float, 3> result;
      for (uint32_t c = 0; c < 3; ++c)
        result[c] = std::exp(-extinction[c] * std::max(distance, 0.f));
      return result;
    }
  };
  inline std::optional<OpticalMedium> measuredMedium(const Material &material) {
    if (!validSmoothBoundary(material))
      return {};
    OpticalMedium result;
    result.refractiveIndex = material.refractiveIndex;
    for (uint32_t c = 0; c < 3; ++c)
      result.extinction[c] =
          -std::log(std::clamp(material.tint[c], 1e-6f, 1.f)) / material.transmittanceDistance;
    return result;
  }
  struct OpticalSheet {
    std::array<float, 3> normalDepth{}; // dimensionless extinction times thickness
    float thicknessRatio = 0; // thickness / authored measurement distance, also applied to tint maps
  };
  inline std::optional<OpticalSheet> measuredSheet(const Material &material) {
    if (!validSmoothBoundary(material) || !(material.thinWallThickness > 0))
      return {};
    OpticalSheet result;
    const double ratio = double(material.thinWallThickness) / material.transmittanceDistance;
    // Every float below one is absorbed completely at this ratio; exact white
    // remains transparent. Keep shader products finite even for extreme units.
    result.thicknessRatio = float(std::min(ratio, 1e10));
    for (uint32_t c = 0; c < 3; ++c)
      result.normalDepth[c] = float(std::min(80.0,
          -std::log(std::clamp(double(material.tint[c]), 1e-6, 1.0)) * ratio));
    return result;
  }
} // namespace remix::scene
