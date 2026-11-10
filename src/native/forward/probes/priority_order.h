#pragma once
#include "forward/probes/data.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace remix::forward::probes {
  // GPU layers use descending priority, with original order retained for ties.
  // Canonical captures/cache data keep their identities and texel order.
  inline std::array<uint32_t, maxProbes> priorityOrder(const Header &header) {
    const auto count = header.parameters[0];
    if (count > maxProbes) throw std::invalid_argument("Invalid reflection priority count");
    std::array<uint32_t, maxProbes> order{};
    std::iota(order.begin(), order.end(), 0u);
    for (uint32_t i = 0; i < count; ++i)
      if (!std::isfinite(header.probes[i].minimum[3]))
        throw std::invalid_argument("Nonfinite reflection priority");
    std::sort(order.begin(), order.begin() + count, [&](uint32_t a, uint32_t b) {
      const auto left = header.probes[a].minimum[3], right = header.probes[b].minimum[3];
      return left != right ? left > right : a < b;
    });
    return order;
  }
}
