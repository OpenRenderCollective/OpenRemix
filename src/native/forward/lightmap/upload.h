#pragma once
#include "forward/lightmap/data.h"

namespace remix::forward::lightmap {
  // Cache/source pages keep all twenty layers. GPU-only compaction removes the
  // local visibility layer after every primary mask has an indexed replacement.
  static_assert(visibilityLayer + 1 == directLayer && sunVisibilityLayer + 1 == atlasLayers);
  constexpr uint32_t indexedVisibilityFlag = 0x80000000u;
  inline uint32_t uploadedLayers(bool indexed) { return atlasLayers - uint32_t(indexed); }
  struct UploadSegment {
    size_t source, destination, values; // uint16 elements, never byte offsets
  };
  inline std::vector<UploadSegment> uploadSegments(const Layout &layout, bool indexed) {
    const size_t layerValues = size_t(layout.pageSize) * layout.pageSize * 4;
    if (!indexed) return {{0, 0, layerValues * layout.pages * atlasLayers}};
    std::vector<UploadSegment> result;
    result.reserve(size_t(layout.pages) * 2);
    for (uint32_t page = 0; page < layout.pages; ++page) {
      const size_t source = size_t(page) * atlasLayers * layerValues;
      const size_t target = size_t(page) * uploadedLayers(true) * layerValues;
      result.push_back({source, target, visibilityLayer * layerValues});
      result.push_back({source + (visibilityLayer + 1) * layerValues, target + visibilityLayer * layerValues,
                        (atlasLayers - visibilityLayer - 1) * layerValues});
    }
    return result;
  }
}
