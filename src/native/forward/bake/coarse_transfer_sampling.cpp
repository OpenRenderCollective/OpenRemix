#include "forward/bake/coarse_transfer.h"
#include <cmath>

namespace remix::forward::bake {
  void applyCoarseTransfer(std::span<const uint32_t> words, const Grid &grid, ProbeStencil &stencil) noexcept {
    if (stencil.count != 16 || !adaptive(grid.header) || words.size() < 4 ||
        words[0] != CoarseTransfer::magic || words[3] != words.size()) return;
    uint32_t nodes = 1;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const auto dimension = grid.header.dimensions[axis];
      if (dimension < 2 || dimension > maximumGridNodes / nodes) return;
      nodes *= dimension;
    }
    if (words[1] != nodes || words.size() < uint64_t(nodes) + 4 || stencil.rootCell >= nodes ||
        grid.lookup.size() < uint64_t(nodes) * 2 + 4) return;
    const auto patch = words[4 + stencil.rootCell];
    if (!patch || patch > words.size() || words.size() - patch < 12) return;
    const auto reference = grid.lookup[nodes + 4 + stencil.rootCell], block = patchOffset(reference);
    if (!block) return;
    const auto factor = patchFactor(reference, grid.lookup[nodes + 1]);
    if (factor != 2 && factor != 4 && factor != 8) return;
    const auto side = factor + 1, count = side * side * side, fields = words[patch + 2];
    if (words[patch] != factor || words[patch + 1] != count || words[patch + 3] != 0 || fields > words.size() ||
        uint64_t(count) * 3 > words.size() - fields || block > grid.lookup.size() ||
        count > grid.lookup.size() - block) return;
    float coarse = 0, fine = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      const float weight = stencil.weights[i];
      if (!std::isfinite(weight) || weight < 0) return;
      (i < 8 ? coarse : fine) += weight;
    }
    if (!std::isfinite(coarse + fine) || coarse <= 1e-6f || fine <= 1e-6f) return;
    const float share = fine / (coarse + fine);
    auto weights = stencil.weights;
    bool occluded = false;
    float total = fine;
    for (uint32_t c = 0; c < 8; ++c) {
      if (weights[c] <= 0) continue;
      uint32_t corner = 8;
      if (stencil.indices[c] < grid.probes.size())
        for (uint32_t i = 0; i < 8; ++i)
          if (words[patch + 4 + i] == stencil.indices[c] + 1) { corner = i; break; }
      float visibility = fine; // Unmatched coarse donors retain ordinary contribution.
      if (corner < 8) {
        visibility = 0;
        for (uint32_t f = 0; f < 8; ++f) {
          const float weight = weights[8 + f];
          if (weight <= 0) continue;
          const auto node = stencil.fineNodes[f];
          float visible = 1;
          if (node < count && stencil.indices[8 + f] < grid.probes.size() &&
              grid.lookup[block + node] == stencil.indices[8 + f] + 1) {
            const auto address = fields + node * 3;
            if ((words[address + 2] & (1u << corner)) != 0) {
              const auto value = (words[address + corner / 4] >> ((corner % 4) * 8)) & 255u;
              visible = float(value) / 255;
              occluded |= value < 255;
            }
          }
          visibility += weight * visible;
        }
      }
      weights[c] *= 1 - share + share * visibility / fine;
      total += weights[c];
    }
    if (!occluded || !std::isfinite(total) || total < 1e-4f) return;
    for (uint32_t i = 0; i < 16; ++i) stencil.weights[i] = weights[i] / total;
  }
}
