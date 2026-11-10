#pragma once
#include "forward/bake/grid.h"
#include "forward/bake/probe_visibility.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::bake {
  struct ProbeVisibilityBudget {
    uint64_t bytes = 0;
    uint32_t pairs = 0;
  };
  // Reserve the shared irradiance grid and worst-case GPU lookup directory
  // first. Directional correction volumes then use their available storage;
  // per-dispatch ray/time limits still control responsiveness while tracing.
  inline ProbeVisibilityBudget probeVisibilityBudget(uint32_t probes, uint32_t lights,
                                                     uint64_t storageRange, bool directional) {
    if (probes == UINT32_MAX || lights >= (1u << 24))
      throw std::invalid_argument("Invalid local probe budget dimensions");
    if (!lights) return {};
    const uint64_t base = sizeof(GridHeader) + uint64_t(probes) * sizeof(Result);
    const uint64_t offsets = (uint64_t(probes) + 1) * sizeof(uint32_t);
    const uint64_t overhead = 32 + ((uint64_t(lights) + 31) / 32) * sizeof(uint32_t) + 15 +
                              ProbeVisibility::maximumDirectoryBytes;
    if (storageRange <= base + offsets + overhead) return {};
    const uint64_t bytes = std::min(ProbeVisibility::maximumBytes, storageRange - base - overhead);
    if (bytes <= offsets) return {};
    const uint64_t stride = sizeof(ProbeVisibility::Entry) + (directional ? sizeof(ProbeVisibility::Direct) : 0);
    const auto capacity = (bytes - offsets) / stride;
    constexpr uint32_t scalarPairLimit = 131072; // Preserve ordinary analytic-light bake work.
    return {bytes, uint32_t(directional ? capacity : std::min<uint64_t>(scalarPairLimit, capacity))};
  }
}
