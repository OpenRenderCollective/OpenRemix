#pragma once
#include <cstdint>

namespace remix::forward::probes {
  // Immutable producer identity for normalization, independent of projection
  // bounds, output fingerprints and image layer order. The provider defines the
  // domain and hashes its frozen lighting inputs/recipe into inputs.
  // Zero in either field is unavailable; matching units alone is insufficient.
  struct ReferenceProvenance {
    uint64_t domain = 0, inputs = 0;
    bool operator==(const ReferenceProvenance &) const = default;
    bool compatible(const ReferenceProvenance &other) const {
      return domain && inputs && *this == other;
    }
  };
}
