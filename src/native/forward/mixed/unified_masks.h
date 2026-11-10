#pragma once
#include "forward/mixed/data.h"
#include <optional>

namespace remix::forward::mixed {
  struct UnifiedMasks {
    Masks masks;
    uint64_t convertedTexels = 0;
    float maximumError = 0;
  };
  // Build one indexed scalar representation from completed primary channels and
  // the existing compressed overflow. Cache arrays are never mutated. Conversion
  // rounds primary half values to R8 (maximum error .5/255), then encodes losslessly.
  // A declined budget keeps the complete legacy layout, never a partial light set.
  std::optional<UnifiedMasks> unifyMasks(const Plan &plan, const lightmap::Layout &layout,
      std::span<const uint16_t> pixels, uint64_t byteBudget, std::stop_token cancelled = {});
}
