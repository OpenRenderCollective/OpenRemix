#include "forward/bake/grid.h"
#include <algorithm>
#include <cmath>

namespace remix::forward::bake {
  namespace {
    void require(bool value) {
      if (!value) throw std::invalid_argument("Invalid irradiance grid directory");
    }
    std::array<uint32_t, 3> coordinate(uint32_t node, const UInt4 &dimensions) {
      std::array<uint32_t, 3> result;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        result[axis] = node % dimensions[axis];
        node /= dimensions[axis];
      }
      return result;
    }
  }
  uint32_t gridNodes(const GridHeader &header) {
    uint32_t count = 1;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      require(header.dimensions[axis] >= 2 && header.dimensions[axis] <= maximumGridNodes / count);
      count *= header.dimensions[axis];
    }
    return count;
  }
  std::vector<uint8_t> sharedPatchFaces(const Grid &grid) {
    const auto &h = grid.header;
    const uint32_t nodes = gridNodes(h);
    std::vector<uint8_t> result(nodes);
    const std::array<uint32_t, 3> strides{1, h.dimensions[0], h.dimensions[0] * h.dimensions[1]};
    for (uint32_t cell = 0; cell < nodes; ++cell) {
      const uint32_t reference = grid.lookup.at(nodes + 4 + cell);
      if (!reference) continue;
      const uint32_t factor = patchFactor(reference, grid.lookup.at(nodes + 1)), side = factor + 1;
      const auto parent = coordinate(cell, h.dimensions);
      for (uint32_t axis = 0; axis < 3; ++axis)
        for (uint32_t high = 0; high < 2; ++high) {
          if ((!high && !parent[axis]) || (high && parent[axis] + 2 >= h.dimensions[axis])) continue;
          const uint32_t neighbour = high ? cell + strides[axis] : cell - strides[axis];
          const uint32_t other = grid.lookup.at(nodes + 4 + neighbour);
          if (!other || patchFactor(other, grid.lookup[nodes + 1]) != factor) continue;
          bool equal = true;
          for (uint32_t u = 0; u < side && equal; ++u)
            for (uint32_t v = 0; v < side; ++v) {
              std::array<uint32_t, 3> a{}, b{};
              a[axis] = high ? factor : 0; b[axis] = high ? 0 : factor;
              a[(axis + 1) % 3] = b[(axis + 1) % 3] = u;
              a[(axis + 2) % 3] = b[(axis + 2) % 3] = v;
              const auto index = [&](const auto &p) { return (p[2] * side + p[1]) * side + p[0]; };
              if (grid.lookup.at(patchOffset(reference) + index(a)) != grid.lookup.at(patchOffset(other) + index(b))) {
                equal = false;
                break;
              }
            }
          if (equal) result[cell] |= uint8_t(1u << (axis * 2 + high));
        }
    }
    return result;
  }
  std::vector<Grid::Origin> directoryOrigins(const Grid &grid) {
    if (!indexed(grid.header)) {
      require(grid.lookup.empty());
      return {};
    }
    const auto &header = grid.header;
    const auto &words = grid.lookup;
    const uint32_t nodes = gridNodes(header), records = header.dimensions[3];
    require(records > 0 && records <= maximumGridNodes && grid.probes.size() == records);
    require(std::isfinite(header.originSpacing[3]) && header.originSpacing[3] > 0);
    for (uint32_t axis = 0; axis < 3; ++axis) require(std::isfinite(header.originSpacing[axis]));
    require(words.size() >= nodes && words.size() <= maximumDirectoryWords);
    std::vector<Grid::Origin> origins(records);
    std::vector<uint8_t> seen(records);
    auto origin = [&](std::array<uint32_t, 3> position, float spacing, uint32_t factor = 1) {
      Grid::Origin result;
      result.spacing = spacing;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        result.position[axis] = header.originSpacing[axis] + position[axis] * spacing;
        require(std::isfinite(result.position[axis]));
      }
      result.relocationSpacing = spacing;
      while (factor > 1 && std::all_of(position.begin(), position.end(), [](auto value) { return value % 2 == 0; })) {
        for (auto &value : position) value /= 2;
        factor /= 2;
        result.relocationSpacing *= 2;
      }
      return result;
    };
    auto own = [&](uint32_t record, Grid::Origin value, bool unique) {
      require(record > 0 && record <= records);
      --record;
      if (seen[record]) {
        require(!unique && origins[record].position == value.position);
        origins[record].spacing = std::min(origins[record].spacing, value.spacing);
        origins[record].relocationSpacing = std::max(origins[record].relocationSpacing, value.relocationSpacing);
      }
      else { origins[record] = value; seen[record] = 1; }
    };
    uint32_t coarseRecords = records;
    if (adaptive(header)) {
      require(words.size() >= size_t(nodes) * 2 + 4 && words[nodes] == adaptiveDirectoryMagic &&
              words[nodes + 2] == words.size());
      coarseRecords = words[nodes + 3];
      require(coarseRecords > 0 && coarseRecords < records);
    } else require(words.size() == nodes);
    for (uint32_t node = 0; node < nodes; ++node) {
      require(words[node] <= coarseRecords);
      if (words[node]) own(words[node], origin(coordinate(node, header.dimensions), header.originSpacing[3]), true);
    }
    if (adaptive(header)) {
      const uint32_t fallback = words[nodes + 1];
      require(fallback == 2 || fallback == 4 || fallback == 8);
      const uint32_t first = (nodes * 2 + 7) & ~3u;
      require(words.size() > first && (words.size() - first) % 4 == 0);
      std::vector<uint8_t> used((words.size() - first) / 4);
      uint32_t patches = 0;
      for (uint32_t word = nodes * 2 + 4; word < first; ++word) require(words[word] == 0);
      for (uint32_t node = 0; node < nodes; ++node) {
        const uint32_t reference = words[nodes + 4 + node];
        if (!reference) continue;
        require(++patches <= maximumRefinedCells);
        const uint32_t factor = patchFactor(reference, fallback), block = patchOffset(reference);
        const uint32_t side = factor + 1, patchNodes = side * side * side, stride = (patchNodes + 3) & ~3u;
        require(block >= first && block + uint64_t(stride) <= words.size());
        for (uint32_t chunk = (block - first) / 4; chunk < (block + stride - first) / 4; ++chunk) {
          require(!used[chunk]);
          used[chunk] = 1;
        }
        const float spacing = header.originSpacing[3] / factor;
        require(std::isfinite(spacing) && spacing > 0);
        const auto base = coordinate(node, header.dimensions);
        for (uint32_t axis = 0; axis < 3; ++axis) require(base[axis] + 1 < header.dimensions[axis]);
        for (uint32_t corner = 0; corner < patchNodes; ++corner) {
          const uint32_t record = words[block + corner];
          if (!record) continue;
          const auto local = coordinate(corner, {side, side, side, 0});
          std::array<uint32_t, 3> position;
          for (uint32_t axis = 0; axis < 3; ++axis) position[axis] = base[axis] * factor + local[axis];
          own(record, origin(position, spacing, factor), false);
        }
        for (uint32_t word = patchNodes; word < stride; ++word) require(words[block + word] == 0);
      }
      require(std::all_of(used.begin(), used.end(), [](auto value) { return value != 0; }));
      const auto shared = sharedPatchFaces(grid);
      const std::array<uint32_t, 3> strides{1, header.dimensions[0], header.dimensions[0] * header.dimensions[1]};
      for (uint32_t cell = 0; cell < nodes; ++cell) {
        const auto flags = patchFaces(words[nodes + 4 + cell]);
        require((flags & ~uint32_t(shared[cell])) == 0);
        for (uint32_t face = 0; face < 6; ++face) {
          if (!(flags & (1u << face))) continue;
          const auto neighbour = (face & 1) ? cell + strides[face / 2] : cell - strides[face / 2];
          require((patchFaces(words[nodes + 4 + neighbour]) & (1u << (face ^ 1u))) != 0);
        }
      }
    }
    require(std::all_of(seen.begin(), seen.end(), [](auto value) { return value != 0; }));
    return origins;
  }
} // namespace remix::forward::bake
