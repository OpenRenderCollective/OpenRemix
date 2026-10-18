#pragma once
#include "forward/lightmap/data.h"

namespace remix::forward::lightmap {
  // Cache/source pages keep all twenty layers. GPU-only compaction removes the
  // local visibility and/or nine direct layers after complete indexed replacements.
  static_assert(visibilityLayer + 1 == directLayer && sunVisibilityLayer + 1 == atlasLayers);
  constexpr uint32_t indexedVisibilityFlag = 0x80000000u;
  inline uint32_t uploadedLayers(bool indexed, bool directional = false) {
    return atlasLayers - uint32_t(indexed) - (directional ? irradianceLayers : 0);
  }
  struct UploadSegment {
    size_t source, destination, values; // uint16 elements, never byte offsets
  };
  inline std::vector<UploadSegment> uploadSegments(const Layout &layout, bool indexed, bool directional = false) {
    const size_t layerValues = size_t(layout.pageSize) * layout.pageSize * 4;
    if (!indexed && !directional) return {{0, 0, layerValues * layout.pages * atlasLayers}};
    std::vector<UploadSegment> result;
    result.reserve(size_t(layout.pages) * 3);
    for (uint32_t page = 0; page < layout.pages; ++page) {
      const size_t source = size_t(page) * atlasLayers * layerValues;
      const size_t target = size_t(page) * uploadedLayers(indexed, directional) * layerValues;
      const size_t prefix = (visibilityLayer + uint32_t(!indexed)) * layerValues;
      result.push_back({source, target, prefix});
      if (!directional)
        result.push_back({source + directLayer * layerValues, target + prefix, irradianceLayers * layerValues});
      result.push_back({source + sunVisibilityLayer * layerValues,
                        target + (uploadedLayers(indexed, directional) - 1) * layerValues, layerValues});
    }
    return result;
  }
}
