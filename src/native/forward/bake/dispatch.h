#pragma once
#include <algorithm>
#include <cstdint>

namespace remix::forward::bake {
  // Vulkan guarantees at least 65535 groups on each compute dispatch axis.
  // A compact rectangle avoids a nearly empty last row at large batch sizes.
  constexpr uint32_t maximumBatchPoints = 4 * 65535;
  struct DispatchShape { uint32_t x, y; };
  inline DispatchShape dispatchShape(uint32_t groups) {
    const uint32_t rows = std::max(1u, (groups + 65534) / 65535);
    return {(groups + rows - 1) / rows, rows};
  }
}
