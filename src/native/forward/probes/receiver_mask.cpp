#include "forward/probes/receiver_mask.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace remix::forward::probes {
  namespace {
    bool overlaps(const Record &probe, const Bounds &bounds, float padding) {
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const auto &row = probe.worldToLocal[axis];
        const double low = probe.influenceMinimum[axis], high = probe.influenceMaximum[axis];
        if (!std::isfinite(low) || !std::isfinite(high) || low > high) return true;
        for (float value : row)
          if (!std::isfinite(value)) return true;
      }
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const auto &row = probe.worldToLocal[axis];
        double low = row[3], high = row[3], magnitude = std::abs(double(row[3]));
        for (uint32_t c = 0; c < 3; ++c) {
          const double a = row[c] * (double(bounds.minimum[c]) - padding);
          const double b = row[c] * (double(bounds.maximum[c]) + padding);
          low += std::min(a, b);
          high += std::max(a, b);
          magnitude += std::max(std::abs(a), std::abs(b));
        }
        // Account for float dot-product rounding, including cancellation at
        // large coordinates. Overflow-prone shader arithmetic stays unculled.
        if (magnitude > std::numeric_limits<float>::max() / 4) return true;
        const double error = 8 * std::numeric_limits<float>::epsilon() *
            (magnitude + std::max(std::abs(double(probe.influenceMinimum[axis])),
                                  std::abs(double(probe.influenceMaximum[axis]))) + 1);
        if (high < double(probe.influenceMinimum[axis]) - error ||
            low > double(probe.influenceMaximum[axis]) + error) return false;
      }
      return true;
    }
  }
  std::array<uint32_t, 2> receiverMask(const Header &header, const Bounds &worldBounds, float padding) {
    bool known = worldBounds.valid && std::isfinite(padding) && padding >= 0;
    for (uint32_t c = 0; c < 3; ++c)
      known &= std::isfinite(worldBounds.minimum[c]) && std::isfinite(worldBounds.maximum[c]) &&
               worldBounds.minimum[c] <= worldBounds.maximum[c];
    std::array<uint32_t, 2> mask{};
    for (uint32_t i = 0; i < std::min(header.parameters[0], maxProbes); ++i)
      if (!known || overlaps(header.probes[i], worldBounds, padding)) mask[i / 32] |= 1u << (i % 32);
    return mask;
  }
}
