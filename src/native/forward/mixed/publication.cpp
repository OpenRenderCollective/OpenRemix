#include "forward/mixed/publication.h"
#include "forward/mixed/unified_masks.h"
#include "forward/mixed/mask_lookup.h"
#include "forward/lightmap/upload.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::mixed {
  namespace {
    uint64_t imageAllocation(VkDevice device, VkExtent2D extent, VkFormat format, uint32_t layers) {
      Image probe;
      probe.device = device;
      const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType = VK_IMAGE_TYPE_2D, .format = format,
          .extent = {extent.width, extent.height, 1}, .mipLevels = 1,
          .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
          .tiling = VK_IMAGE_TILING_OPTIMAL,
          .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
      check(vkCreateImage(device, &info, nullptr, &probe.handle));
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, probe.handle, &requirements);
      return requirements.size;
    }
    uint64_t atlasAllocation(VkDevice device, const lightmap::Layout &layout, bool indexed, bool directional = false) {
      return layout.pages ? imageAllocation(device, {layout.pageSize, layout.pageSize},
          VK_FORMAT_R16G16B16A16_SFLOAT, layout.pages * lightmap::uploadedLayers(indexed, directional)) : 0;
    }
    void allocate(VisibilityPublication &result, VkPhysicalDevice physical, VkDevice device) {
      result.lookupImage = std::make_unique<OcclusionMap>(physical, device,
          VkExtent2D{result.lookup.width, result.lookup.height});
      if (result.pages)
        result.pageImage = std::make_unique<OcclusionMap>(physical, device,
            VkExtent2D{result.pages->size, result.pages->size}, VK_FORMAT_R8_UNORM, result.pages->layers);
    }
    VisibilityPublication candidate(VkDevice device, const Plan &plan,
        const Masks &masks, const VkPhysicalDeviceLimits &limits, bool allowPages, uint64_t budget,
        std::span<const uint32_t> directional = {}) {
      VisibilityPublication result;
      const auto charts = uint32_t(plan.charts.size());
      auto words = masks.words(charts);
      result.hasMasks = !words.empty();
      indexMaskRecords(words, charts);
      const auto appendDirectional = [&](std::vector<uint32_t> &tail) {
        const auto first = uint64_t(2) + plan.occlusion.words().size() + tail.size();
        if (first + directional.size() > UINT32_MAX)
          throw std::length_error("Directional lookup exceeds word addressing");
        const auto offset = uint32_t(first);
        tail.insert(tail.end(), directional.begin(), directional.end());
        return directional.empty() ? 0u : offset;
      };
      result.directionalOffset = appendDirectional(words);
      result.lookup = plan.occlusion.pixels(limits.maxImageDimension2D, words);
      result.compressedAllocation = imageAllocation(device, {result.lookup.width, result.lookup.height}, VK_FORMAT_R32_UINT, 1);
      uint64_t lookupAllocation = result.compressedAllocation, pageAllocation = 0;
      if (allowPages) result.pages = selectMaskPages(masks, charts, limits.maxImageDimension2D,
                                                     limits.maxImageArrayLayers, {}, budget);
      if (result.pages) {
        auto pageWords = result.pages->words(charts);
        indexMaskRecords(pageWords, charts);
        const auto directionalOffset = appendDirectional(pageWords);
        auto lookup = plan.occlusion.pixels(limits.maxImageDimension2D, pageWords);
        const auto lookupBytes = imageAllocation(device, {lookup.width, lookup.height}, VK_FORMAT_R32_UINT, 1);
        const auto pageBytes = imageAllocation(device, {result.pages->size, result.pages->size},
            VK_FORMAT_R8_UNORM, result.pages->layers);
        if (lookupBytes + pageBytes < result.compressedAllocation) {
          result.lookup = std::move(lookup);
          lookupAllocation = lookupBytes;
          pageAllocation = pageBytes;
          result.directionalOffset = directionalOffset;
        } else result.pages.reset();
      }
      result.totalAllocation = lookupAllocation + pageAllocation;
      return result;
    }
  }
  VisibilityPublication prepareVisibility(VkPhysicalDevice physical, VkDevice device,
      const Plan &plan, const lightmap::Layout &layout, std::span<const uint16_t> pixels,
      bool allowPages, const UnifiedMasks *unified, std::span<const uint32_t> directional) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8_UNORM, &format);
    allowPages &= (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    auto legacy = candidate(device, plan, plan.masks, properties.limits, allowPages, Masks::maximumBytes);
    const uint64_t legacyAtlas = pixels.empty() ? 0 : atlasAllocation(device, layout, false);
    legacy.totalAllocation += legacyAtlas;
    legacy.legacyAllocation = legacy.totalAllocation;
    const uint64_t baseline = legacy.totalAllocation;
    for (uint32_t mode = 1; !pixels.empty() && mode < 4; ++mode) {
      const bool masksIndexed = (mode & 1u) != 0, directIndexed = (mode & 2u) != 0;
      if ((masksIndexed && !unified) || (directIndexed && directional.empty())) continue;
      try {
        auto next = candidate(device, plan, masksIndexed ? unified->masks : plan.masks,
            properties.limits, allowPages, masksIndexed ? Masks::maximumPublishedBytes : Masks::maximumBytes,
            directIndexed ? directional : std::span<const uint32_t>{});
        next.totalAllocation += atlasAllocation(device, layout, masksIndexed, directIndexed);
        if (next.totalAllocation >= legacy.totalAllocation) continue;
        next.indexed = masksIndexed;
        next.indexedDirect = directIndexed;
        next.legacyAllocation = baseline;
        next.convertedTexels = masksIndexed ? unified->convertedTexels : 0;
        next.maximumError = masksIndexed ? unified->maximumError : 0;
        legacy = std::move(next);
      } catch (const std::length_error &) {
        // Device lookup capacity can be tighter than the worker's encoding
        // budget. Keep the complete best candidate rather than partial fields.
      } catch (VkResult error) {
        if (error != VK_ERROR_OUT_OF_DEVICE_MEMORY && error != VK_ERROR_OUT_OF_HOST_MEMORY) throw;
      }
    }
    // All comparisons used unbound images. Allocate only the winning lookup
    // and optional R8 pages, rather than several large directional candidates.
    try {
      allocate(legacy, physical, device);
    } catch (VkResult error) {
      if (error != VK_ERROR_OUT_OF_DEVICE_MEMORY && error != VK_ERROR_OUT_OF_HOST_MEMORY) throw;
      legacy.lookupImage.reset();
      legacy.pageImage.reset();
      if (legacy.indexedDirect)
        return prepareVisibility(physical, device, plan, layout, pixels, allowPages, unified, {});
      if (legacy.indexed)
        return prepareVisibility(physical, device, plan, layout, pixels, allowPages, nullptr, {});
      throw;
    }
    return legacy;
  }
}
