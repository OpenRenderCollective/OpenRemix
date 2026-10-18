#include "forward/mixed/publication.h"
#include "forward/mixed/unified_masks.h"
#include "forward/mixed/mask_lookup.h"
#include "forward/lightmap/upload.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::mixed {
  namespace {
    uint64_t atlasAllocation(VkDevice device, const lightmap::Layout &layout, bool indexed) {
      if (!layout.pages) return 0;
      Image probe;
      probe.device = device;
      const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
          .extent = {layout.pageSize, layout.pageSize, 1}, .mipLevels = 1,
          .arrayLayers = layout.pages * lightmap::uploadedLayers(indexed), .samples = VK_SAMPLE_COUNT_1_BIT,
          .tiling = VK_IMAGE_TILING_OPTIMAL,
          .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
      check(vkCreateImage(device, &info, nullptr, &probe.handle));
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, probe.handle, &requirements);
      return requirements.size;
    }
    VisibilityPublication candidate(VkPhysicalDevice physical, VkDevice device, const Plan &plan,
        const Masks &masks, const VkPhysicalDeviceLimits &limits, bool allowPages, uint64_t budget) {
      VisibilityPublication result;
      const auto charts = uint32_t(plan.charts.size());
      auto words = masks.words(charts);
      result.hasMasks = !words.empty();
      indexMaskRecords(words, charts);
      result.lookup = plan.occlusion.pixels(limits.maxImageDimension2D, words);
      result.lookupImage = std::make_unique<OcclusionMap>(physical, device,
          VkExtent2D{result.lookup.width, result.lookup.height});
      result.compressedAllocation = result.lookupImage->bytes();
      if (allowPages) result.pages = selectMaskPages(masks, charts, limits.maxImageDimension2D,
                                                     limits.maxImageArrayLayers, {}, budget);
      if (result.pages) {
        auto pageWords = result.pages->words(charts);
        indexMaskRecords(pageWords, charts);
        auto lookup = plan.occlusion.pixels(limits.maxImageDimension2D, pageWords);
        auto lookupImage = std::make_unique<OcclusionMap>(physical, device,
            VkExtent2D{lookup.width, lookup.height});
        auto pageImage = std::make_unique<OcclusionMap>(physical, device,
            VkExtent2D{result.pages->size, result.pages->size}, VK_FORMAT_R8_UNORM, result.pages->layers);
        if (lookupImage->bytes() + pageImage->bytes() < result.compressedAllocation) {
          result.lookup = std::move(lookup);
          result.lookupImage = std::move(lookupImage);
          result.pageImage = std::move(pageImage);
        } else result.pages.reset();
      }
      result.totalAllocation = result.lookupImage->bytes() + (result.pageImage ? result.pageImage->bytes() : 0);
      return result;
    }
  }
  VisibilityPublication prepareVisibility(VkPhysicalDevice physical, VkDevice device,
      const Plan &plan, const lightmap::Layout &layout, std::span<const uint16_t> pixels,
      bool allowPages, const UnifiedMasks *unified) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8_UNORM, &format);
    allowPages &= (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    auto legacy = candidate(physical, device, plan, plan.masks, properties.limits, allowPages, Masks::maximumBytes);
    const uint64_t legacyAtlas = pixels.empty() ? 0 : atlasAllocation(device, layout, false);
    legacy.totalAllocation += legacyAtlas;
    legacy.legacyAllocation = legacy.totalAllocation;
    if (!unified || pixels.empty()) return legacy;
    try {
      auto indexed = candidate(physical, device, plan, unified->masks, properties.limits,
                                 allowPages, Masks::maximumPublishedBytes);
      indexed.totalAllocation += atlasAllocation(device, layout, true);
      if (indexed.totalAllocation >= legacy.totalAllocation) return legacy;
      indexed.indexed = true;
      indexed.legacyAllocation = legacy.totalAllocation;
      indexed.convertedTexels = unified->convertedTexels;
      indexed.maximumError = unified->maximumError;
      return indexed;
    } catch (const std::length_error &) {
      // Device lookup capacity can be tighter than the CPU encoding budget.
      // The untouched legacy candidate remains complete and usable.
      return legacy;
    }
  }
}
