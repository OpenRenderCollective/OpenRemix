#pragma once
#include "forward/mixed/data.h"

namespace remix::forward::mixed {
  // Completed-plan accounting. Rectangle areas include chart padding, not just
  // valid surface luxels. Unknown links never count as positive coverage.
  struct OwnershipSummary {
    struct Chart {
      uint32_t width, height, retained, knownPositive, clear;
    };
    uint64_t allocatedDirectionalBytes = 0;
    uint64_t retainedRectangleBytes = 0, knownPositiveRectangleBytes = 0;
    uint64_t retainedPairs = 0, knownPositivePairs = 0, clearPairs = 0;
    uint64_t clearRectangleBytes = 0;
    std::array<uint64_t, channels + 1> chartsByRetainedCount{};
    std::vector<Chart> charts;
  };
  OwnershipSummary summarizeOwnership(const Plan &plan, const lightmap::Layout &layout,
                                        std::stop_token cancelled = {});
  // Optional diagnostic only. Refuses to overwrite a prior report.
  void dumpOwnership(const Plan &plan, const lightmap::Layout &layout) noexcept;
}
