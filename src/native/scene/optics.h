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
} // namespace remix::scene
