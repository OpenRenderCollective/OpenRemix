#pragma once
#include <cstdint>

namespace remix::config {
  // Numeric values are part of the native capability/configuration contract.
  // Keep them stable while using names for internal routing decisions.
  struct Backend {
    static constexpr uint32_t Raster = 0, PathTrace = 1, ForwardPlus = 2;
    static constexpr const char *name(uint32_t value) {
      return value == ForwardPlus ? "Forward+" : value == PathTrace ? "Path trace" : "Raster";
    }
  };
} // namespace remix::config
