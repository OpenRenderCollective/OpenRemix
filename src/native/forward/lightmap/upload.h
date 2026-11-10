#pragma once
#include "forward/lightmap/data.h"

namespace remix::forward::lightmap {
  // Cache/source pages keep all twenty layers. GPU-only compaction removes the
  // local visibility and/or nine direct layers after complete indexed replacements.
  static_assert(visibilityLayer == irradianceLayers && directLayer == visibilityLayer + 1 &&
                sunVisibilityLayer == directLayer + irradianceLayers && atlasLayers == sunVisibilityLayer + 1);
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
    // Removed layers form one contiguous span. Keep the prefix and suffix.
    const uint32_t firstRemoved = indexed ? visibilityLayer : directLayer;
    const uint32_t firstRetained = directional ? sunVisibilityLayer : directLayer;
    const size_t prefix = firstRemoved * layerValues;
    const size_t suffix = (atlasLayers - firstRetained) * layerValues;
    const size_t targetPageValues = uploadedLayers(indexed, directional) * layerValues;
    std::vector<UploadSegment> result;
    result.reserve(size_t(layout.pages) * 2);
    for (uint32_t page = 0; page < layout.pages; ++page) {
      const size_t source = size_t(page) * atlasLayers * layerValues;
      const size_t target = size_t(page) * targetPageValues;
      result.push_back({source, target, prefix});
      result.push_back({source + firstRetained * layerValues, target + prefix, suffix});
    }
    return result;
  }
}
