#pragma once

#include "forward/probes/data.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace remix::forward::probes {
  // Six words store signed cell origin and unsigned dimensions, followed by
  // two mask words per cell, X-major. Translated boxes qualify for this
  // conservative lookup. Other publications use the original draw mask.
  inline std::vector<uint32_t> buildSpatialMask(const Header &header, size_t byteBudget) {
    constexpr double cellSize = 128;
    constexpr double coordinateLimit = 1048576;
    // The only nonzero terms in each shader dot are the world coordinate
    // and translation. Within this domain their float sum rounds by much
    // less than four units, even at the outermost grid cells.
    constexpr double slack = 4;
    constexpr size_t maxCells = 512 * 1024;
    const uint32_t count = header.parameters[0];
    if (!count || count > maxProbes) return {};
    using Cell = std::array<int32_t, 3>;
    std::array<Cell, maxProbes> lows{}, highs{};
    Cell origin, last;
    origin.fill(std::numeric_limits<int32_t>::max());
    last.fill(std::numeric_limits<int32_t>::min());
    for (uint32_t index = 0; index < count; ++index) {
      const auto &probe = header.probes[index];
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const auto &row = probe.worldToLocal[axis];
        for (uint32_t column = 0; column < 3; ++column)
          if (row[column] != float(axis == column)) return {};
        const double lo = probe.influenceMinimum[axis], hi = probe.influenceMaximum[axis], t = row[3];
        if (!(std::abs(lo) <= coordinateLimit && std::abs(hi) <= coordinateLimit &&
              std::abs(t) <= coordinateLimit && lo <= hi)) return {};
        lows[index][axis] = int32_t(std::floor((lo - t - slack) / cellSize));
        highs[index][axis] = int32_t(std::floor((hi - t + slack) / cellSize));
        origin[axis] = std::min(origin[axis], lows[index][axis]);
        last[axis] = std::max(last[axis], highs[index][axis]);
      }
    }
    Cell dims{};
    for (uint32_t axis = 0; axis < 3; ++axis) dims[axis] = last[axis] - origin[axis] + 1;
    const size_t cells = size_t(dims[0]) * dims[1] * dims[2];
    if (cells > maxCells || (6 + 2 * cells) * sizeof(uint32_t) > byteBudget) return {};
    std::vector<uint32_t> words(6 + 2 * cells);
    for (uint32_t axis = 0; axis < 3; ++axis) {
      words[axis] = uint32_t(origin[axis]);
      words[axis + 3] = uint32_t(dims[axis]);
    }
    for (uint32_t index = 0; index < count; ++index) {
      const auto &lo = lows[index], &hi = highs[index];
      for (int32_t z = lo[2] - origin[2]; z <= hi[2] - origin[2]; ++z)
        for (int32_t y = lo[1] - origin[1]; y <= hi[1] - origin[1]; ++y)
          for (int32_t x = lo[0] - origin[0]; x <= hi[0] - origin[0]; ++x) {
            const size_t cell = x + dims[0] * (y + dims[1] * z);
            words[6 + 2 * cell + index / 32] |= 1u << (index % 32);
          }
    }
    return words;
  }
}
