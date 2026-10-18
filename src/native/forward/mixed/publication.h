#pragma once
#include "forward/mixed/map.h"
#include "forward/mixed/mask_pages.h"
#include "forward/mixed/data.h"
#include "forward/mixed/unified_masks.h"

namespace remix::forward::mixed {
  struct VisibilityPublication {
    Occlusion::Pixels lookup;
    std::unique_ptr<OcclusionMap> lookupImage, pageImage;
    std::optional<MaskPages> pages;
    bool indexed = false, indexedDirect = false, hasMasks = false;
    uint32_t directionalOffset = 0; // R32_UINT word offset, zero keeps atlas SH
    uint64_t compressedAllocation = 0, legacyAllocation = 0, totalAllocation = 0;
    uint64_t convertedTexels = 0;
    float maximumError = 0;
  };
  // Chooses complete replacements before upload. All candidate image memory is
  // queried unbound; only the winning lookup/pages receive GPU allocations.
  VisibilityPublication prepareVisibility(VkPhysicalDevice physical, VkDevice device,
      const Plan &plan, const lightmap::Layout &layout, std::span<const uint16_t> pixels,
      bool allowPages, const UnifiedMasks *unified, std::span<const uint32_t> directional = {});
}
