#pragma once
#include "forward/bake/grid.h"
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  // Canonical admitted position from retained/cache records, without allocating
  // the transient Point array. Relocation offsets are already in scene units.
  inline Vec3 probeOrigin(const Grid &grid, uint32_t index) {
    const auto &header = grid.header;
    if (index >= grid.probes.size() || !header.dimensions[0] || !header.dimensions[1] ||
        (indexed(header) && index >= grid.origins.size()))
      throw std::invalid_argument("Invalid admitted probe origin");
    const auto plane = uint64_t(header.dimensions[0]) * header.dimensions[1];
    const uint32_t coordinate[] = {index % header.dimensions[0],
        (index / header.dimensions[0]) % header.dimensions[1], uint32_t(index / plane)};
    Vec3 position;
    for (uint32_t c = 0; c < 3; ++c) {
      position[c] = indexed(header) ? grid.origins[index].position[c]
          : header.originSpacing[c] + coordinate[c] * header.originSpacing[3];
      if (uint32_t(header.settings[2]) & 1u) position[c] += grid.probes[index].irradiance[c][3];
      if (!std::isfinite(position[c])) throw std::invalid_argument("Nonfinite admitted probe origin");
    }
    return position;
  }
}
