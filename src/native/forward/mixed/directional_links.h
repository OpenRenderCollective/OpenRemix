#pragma once
#include <cstdint>
#include <stop_token>
#include <vector>

namespace remix::forward::mixed {
  struct Plan;
  struct DirectionalLink {
    uint32_t chart, light;
    bool operator==(const DirectionalLink &) const = default;
  };
  // Sorted unique chart/light pairs with positive sampled visibility. Unknown
  // coverage is never promoted to ownership. Retained direct owners are kept
  // even when their captured response is black.
  // The budget bounds vector storage before deduplication; exhaustion throws
  // without exposing a partial manifest.
  std::vector<DirectionalLink> positiveDirectionalLinks(const Plan &plan,
      std::stop_token cancelled = {}, uint64_t byteBudget = 128ull << 20);
}
