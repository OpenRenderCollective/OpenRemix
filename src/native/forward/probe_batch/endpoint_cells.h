#pragma once
#include "forward/bake/grid.h"
#include <cstdlib>

namespace remix::forward::probe_batch {
  inline bool endpointCellsRequested() {
    static const bool enabled = std::getenv("REMIX_DEBUG_PROBE_ENDPOINT_CELLS") != nullptr;
    return enabled;
  }

  // Keys are lower-corner node addresses in the ordinary probe directory.
  // Zero marks an address that is not a cell. One selects the root grid's
  // dimensions, and a larger value is a cubic refinement patch's side.
  inline std::vector<uint32_t> endpointCellSides(const bake::Grid &grid) {
    const auto nodes = bake::gridNodes(grid.header);
    const auto count = bake::indexed(grid.header) ? grid.lookup.size() : size_t(nodes);
    if (count == 0 || count > (1u << 20))
      return {};
    std::vector<uint32_t> sides(count);
    const auto mark = [&](uint32_t offset, uint32_t x, uint32_t y, uint32_t z, uint32_t side) {
      for (uint32_t k = 0; k + 1 < z; ++k)
        for (uint32_t j = 0; j + 1 < y; ++j)
          for (uint32_t i = 0; i + 1 < x; ++i)
            sides[offset + (k * y + j) * x + i] = side;
    };
    const auto &dimensions = grid.header.dimensions;
    mark(0, dimensions[0], dimensions[1], dimensions[2], 1);
    if (bake::adaptive(grid.header))
      for (uint32_t cell = 0; cell < nodes; ++cell) {
        const auto reference = grid.lookup[nodes + 4 + cell];
        const auto offset = bake::patchOffset(reference);
        if (!offset) continue;
        const auto side = bake::patchFactor(reference, grid.lookup[nodes + 1]) + 1;
        mark(offset, side, side, side, side);
      }
    return sides;
  }
}
