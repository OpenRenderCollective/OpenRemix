#pragma once
#include "forward/probes/composition.h"
#include "forward/probes/priority_order.h"

namespace remix::forward::probes {
  // CPU publication metadata only. Zero denotes automatic/unbound layers; the
  // GPU header, capture pixels, and diffuse/cache identities remain unchanged.
  inline LayerIdentities imageIdentities(const Header &canonical, std::span<const uint64_t> ids) {
    if (ids.size() != canonical.parameters[0])
      throw std::invalid_argument("Reflection identity count differs from capture count");
    const auto order = priorityOrder(canonical);
    LayerIdentities result{};
    for (size_t layer = 0; layer < ids.size(); ++layer) {
      const auto id = ids[order[layer]];
      if (!id || std::find(result.begin(), result.begin() + layer, id) != result.begin() + layer)
        throw std::invalid_argument("Authored reflection identities must be nonzero and unique");
      result[layer] = id;
    }
    return result;
  }

  inline LayerIdentities composedIdentities(const Composition &plan, std::span<const uint64_t> authored) {
    if (plan.layers.size() != plan.header.parameters[0] || plan.layers.size() > maxProbes)
      throw std::invalid_argument("Reflection identity composition has invalid layer count");
    LayerIdentities result{};
    for (size_t layer = 0; layer < plan.layers.size(); ++layer) {
      const auto source = plan.layers[layer];
      if (source.automatic) continue;
      if (source.layer >= authored.size() || !authored[source.layer] ||
          std::find(result.begin(), result.begin() + layer, authored[source.layer]) != result.begin() + layer)
        throw std::invalid_argument("Reflection identity composition has invalid authored layer");
      result[layer] = authored[source.layer];
    }
    return result;
  }
}
