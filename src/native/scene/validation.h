#pragma once
#include "scene/types.h"
#include <array>
#include <cmath>
#include <remix/remix_native_ext.h>

namespace remix::scene::detail {
  constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  constexpr auto unavailable = REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
  template <typename T> uint64_t key(T handle) { return reinterpret_cast<uintptr_t>(handle); }
  // Inline: called for every vertex of every mesh a host creates.
  inline void finite(const float *values, size_t count) {
    for (size_t i = 0; i < count; ++i)
      if (!std::isfinite(values[i]))
        throw invalid;
  }
  // value / 255 for each byte value (the same quotients as the division).
  inline const std::array<float, 256> &unitBytes() {
    static const std::array<float, 256> table = [] {
      std::array<float, 256> out{};
      for (int i = 0; i < 256; ++i)
        out[i] = float(i) / 255;
      return out;
    }();
    return table;
  }
  inline void rgba(uint32_t color, float *out, bool ui = false) {
    const auto &unit = unitBytes();
    out[0] = unit[(color >> (ui ? 0 : 16)) & 255];
    out[1] = unit[(color >> 8) & 255];
    out[2] = unit[(color >> (ui ? 16 : 0)) & 255];
    out[3] = unit[color >> 24];
  }
  Blend blend(const remixapi_InstanceInfoBlendEXT *info);
} // namespace remix::scene::detail
