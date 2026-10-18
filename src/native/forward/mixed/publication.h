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
    bool indexed = false, hasMasks = false;
    uint64_t compressedAllocation = 0, legacyAllocation = 0, totalAllocation = 0;
    uint64_t convertedTexels = 0;
    float maximumError = 0;
  };
  // Chooses a complete representation before upload. Surface memory is queried
  // using unbound images, so comparison never allocates two large atlases.
  VisibilityPublication prepareVisibility(VkPhysicalDevice physical, VkDevice device,
      const Plan &plan, const lightmap::Layout &layout, std::span<const uint16_t> pixels,
      bool allowPages, const UnifiedMasks *unified);
}
