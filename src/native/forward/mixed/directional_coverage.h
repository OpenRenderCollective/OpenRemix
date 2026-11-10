#pragma once
#include "forward/mixed/directional_fields.h"

namespace remix::forward::mixed {
  // Validate both the encoded fields and exact coverage of the positive
  // chart/light manifest. Generic codec validity alone cannot prove ownership.
  void validateDirectionalCoverage(std::span<const uint32_t> words, const Plan &plan,
      const lightmap::Layout &layout, std::stop_token cancelled = {});
}
