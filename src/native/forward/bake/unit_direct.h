#pragma once
#include "forward/half.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  // Signed, cosine-convolved L2 irradiance of one unit-radiance emitter.
  // Light identity and scalar visibility belong to the enclosing sparse table.
  using UnitDirect = std::array<float, 9>;
  // Five half pairs (last half zero), then a full-precision amplitude. Scaling
  // before half conversion preserves finite HDR and subnormal input ranges
  // without the saturation used by the display-oriented lightmap half helper.
  using PackedUnitDirect = std::array<uint32_t, 6>;
  static_assert(sizeof(PackedUnitDirect) == 24);

  inline PackedUnitDirect packUnitDirect(const UnitDirect &coefficients) {
    float amplitude = 0;
    for (float value : coefficients) {
      if (!std::isfinite(value)) throw std::invalid_argument("Non-finite unit-direct coefficient");
      amplitude = std::max(amplitude, std::abs(value));
    }
    PackedUnitDirect packed{};
    packed[5] = std::bit_cast<uint32_t>(amplitude);
    for (uint32_t i = 0; i < coefficients.size(); ++i) {
      const float normalized = amplitude > 0 ? coefficients[i] / amplitude : coefficients[i];
      packed[i / 2] |= uint32_t(half::encode(normalized)) << ((i & 1u) * 16);
    }
    return packed;
  }

  inline UnitDirect unpackUnitDirect(const PackedUnitDirect &packed) {
    const float amplitude = std::bit_cast<float>(packed[5]);
    if (!std::isfinite(amplitude) || amplitude < 0 || (packed[4] >> 16) != 0)
      throw std::invalid_argument("Invalid packed unit-direct record");
    UnitDirect result{};
    for (uint32_t i = 0; i < result.size(); ++i) {
      const float normalized = half::decode(uint16_t(packed[i / 2] >> ((i & 1u) * 16)));
      if (std::abs(normalized) > 1)
        throw std::invalid_argument("Invalid normalized unit-direct coefficient");
      result[i] = normalized * amplitude;
    }
    return result;
  }

  inline bool zeroUnitDirect(const UnitDirect &coefficients) {
    return std::all_of(coefficients.begin(), coefficients.end(), [](float value) { return value == 0; });
  }
}
