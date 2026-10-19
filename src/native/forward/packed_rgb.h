#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace remix::forward {
  // Signed nine-bit RGB mantissas share a five-bit power-of-two exponent.
  // One SH coefficient takes four bytes instead of three half floats. Keeping
  // an exponent per coefficient preserves weak bands beside a bright DC band.
  inline uint32_t packSignedRgb(std::array<float, 3> value) {
    float largest = 0;
    for (auto &channel : value) {
      if (!std::isfinite(channel)) throw std::invalid_argument("Non-finite packed irradiance");
      channel = std::clamp(channel, -65000.f, 65000.f);
      largest = std::max(largest, std::abs(channel));
    }
    if (largest < std::ldexp(1.f, -23)) return 0;
    int exponent = std::max(-22, std::ilogb(largest / 255.f));
    if (std::ldexp(255.f, exponent) < largest) ++exponent;
    const float step = std::ldexp(1.f, exponent);
    uint32_t word = uint32_t(exponent + 22) << 27;
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const int mantissa = std::clamp(int(std::round(value[channel] / step)), -255, 255);
      word |= (uint32_t(mantissa) & 511u) << (channel * 9);
    }
    return word;
  }
  inline std::array<float, 3> unpackSignedRgb(uint32_t word) {
    const float step = std::bit_cast<float>(((word >> 27) + 105u) << 23);
    std::array<float, 3> value;
    for (uint32_t channel = 0; channel < 3; ++channel) {
      int mantissa = int((word >> (channel * 9)) & 511u);
      if (mantissa >= 256) mantissa -= 512;
      value[channel] = float(mantissa) * step;
    }
    return value;
  }
  // Exact canonicality check, equivalent to pack(unpack(word)) == word.
  // Encoding never emits -256, denormalized mantissas above exponent zero,
  // or exponent 31. At exponent 30 the HDR clamp rounds to at most 254.
  inline bool canonicalSignedRgb(uint32_t word) noexcept {
    const uint32_t exponent = word >> 27;
    if (exponent > 30) return false;
    uint32_t largest = 0;
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const uint32_t bits = (word >> (channel * 9)) & 511u;
      if (bits == 256) return false;
      largest = std::max(largest, bits >= 256 ? 512 - bits : bits);
    }
    if (largest == 0) return exponent == 0;
    return (exponent == 0 || largest >= 128) && (exponent < 30 || largest <= 254);
  }

}
