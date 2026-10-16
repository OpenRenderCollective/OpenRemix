#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace remix::forward::probes::detail {
  // Conservative half-space clipping. Oblique planes can overestimate the
  // surviving cell; consumers still validate origins or complete box occupancy.
  // Callers validate finite, nonzero planes before using this shared primitive.
  struct Box {
    std::array<double, 3> minimum, maximum;
  };
  inline bool clip(Box &box, const float (&plane)[4], double sign) {
    const auto original = box;
    double maximum = 0;
    for (uint32_t c = 0; c < 3; ++c) {
      const double n = sign * plane[c];
      maximum += n * (n >= 0 ? original.maximum[c] : original.minimum[c]);
    }
    const double distance = sign * plane[3];
    if (maximum < distance)
      return false;
    for (uint32_t c = 0; c < 3; ++c) {
      const double n = sign * plane[c];
      if (!n)
        continue;
      const double contribution = n * (n > 0 ? original.maximum[c] : original.minimum[c]);
      const double boundary = (distance - (maximum - contribution)) / n;
      if (n > 0)
        box.minimum[c] = std::max(box.minimum[c], boundary);
      else
        box.maximum[c] = std::min(box.maximum[c], boundary);
    }
    for (uint32_t c = 0; c < 3; ++c)
      if (box.maximum[c] <= box.minimum[c])
        return false;
    return true;
  }
} // namespace remix::forward::probes::detail
