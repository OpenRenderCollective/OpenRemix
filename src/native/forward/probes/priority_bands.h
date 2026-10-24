#pragma once
#include "forward/probes/data.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <stdexcept>

namespace remix::forward::probes {
  // Derived publication metadata, separate from canonical capture/cache records.
  // Each face has its own inverse priority width. Spatial blending continues
  // to use the authored per-axis widths in Record.
  struct PriorityBands {
    std::array<Vec4, maxProbes> minimum{}, maximum{};
  };
  struct GpuHeader {
    Header captures;
    PriorityBands priority;
  };
  static_assert(sizeof(GpuHeader) == sizeof(Header) + 2048 &&
                offsetof(GpuHeader, priority) == sizeof(Header));

  inline PriorityBands priorityBands(const Header &header) {
    const uint32_t count = header.parameters[0];
    if (count > maxProbes) throw std::invalid_argument("Invalid reflection priority count");
    PriorityBands result;
    bool layered = false;
    for (uint32_t i = 1; i < count; ++i)
      layered |= header.probes[i].minimum[3] != header.probes[0].minimum[3];
    if (!layered) return result; // Equal-priority sets never read the derived bands.
    for (uint32_t i = 0; i < count; ++i) {
      const auto &record = header.probes[i];
      const Vec3 inverse{record.maximum[3], record.influenceMinimum[3], record.influenceMaximum[3]};
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const float half = (record.influenceMaximum[axis] - record.influenceMinimum[axis]) * .5f;
        if (!std::isfinite(half) || half <= 0 || !std::isfinite(inverse[axis]))
          throw std::invalid_argument("Invalid reflection priority feather width");
        result.minimum[i][axis] = result.maximum[i][axis] = inverse[axis] > 0 ? inverse[axis] : 1 / (half * .2f);
      }
    }
    // Equal outward normals and plane offsets identify the same boundary in
    // world space, even with permuted local axes. Exact equality deliberately
    // leaves nearby bevels and distinct thin walls separate.
    using Plane = std::array<float, 4>;
    const auto plane = [](const Record &record, uint32_t axis, bool maximum) {
      const auto &row = record.worldToLocal[axis];
      const float sign = maximum ? 1.f : -1.f;
      const float bound = maximum ? record.influenceMaximum[axis] : record.influenceMinimum[axis];
      Plane key{sign * row[0], sign * row[1], sign * row[2], sign * (bound - row[3])};
      if (!std::all_of(key.begin(), key.end(), [](float x) { return std::isfinite(x); }))
        throw std::invalid_argument("Invalid reflection priority boundary");
      return key;
    };
    std::map<Plane, float> shared;
    for (uint32_t i = 0; i < count; ++i)
      for (uint32_t axis = 0; axis < 3; ++axis)
        for (bool maximum : {false, true}) {
          const float inverseWidth = maximum ? result.maximum[i][axis] : result.minimum[i][axis];
          auto &canonical = shared[plane(header.probes[i], axis, maximum)];
          canonical = std::max(canonical, inverseWidth);
        }
    for (uint32_t i = 0; i < count; ++i)
      for (uint32_t axis = 0; axis < 3; ++axis) {
        result.minimum[i][axis] = shared.at(plane(header.probes[i], axis, false));
        result.maximum[i][axis] = shared.at(plane(header.probes[i], axis, true));
      }
    return result;
  }
} // namespace remix::forward::probes
