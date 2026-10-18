#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace remix::forward::half {
  inline uint16_t encode(float value) {
    if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite lighting coefficient");
    const uint32_t sign = std::bit_cast<uint32_t>(value) >> 16 & 0x8000u;
    const float magnitude = std::min(std::abs(value), 65000.f);
    if (magnitude < 6.103515625e-5f)
      return uint16_t(sign | uint32_t(std::lround(magnitude / 5.960464477539063e-8f)));
    const uint32_t bits = std::bit_cast<uint32_t>(magnitude) + 0x1000u;
    return uint16_t(sign | (((bits >> 23) - 112) << 10) | (bits >> 13 & 0x3ffu));
  }
  inline float decode(uint16_t value) {
    const int exponent = (value >> 10) & 31;
    if (exponent == 31) throw std::invalid_argument("Nonfinite lighting half coefficient");
    const float mantissa = float(value & 1023);
    const float magnitude = exponent ? std::ldexp(1024 + mantissa, exponent - 25)
                                     : std::ldexp(mantissa, -24);
    return value & 0x8000 ? -magnitude : magnitude;
  }
} // namespace remix::forward::half
