#include "forward/mixed/publication.h"
#include "forward/mixed/unified_masks.h"
#include "forward/mixed/mask_lookup.h"
#include "forward/mixed/directional_fields.h"
#include "forward/lightmap/upload.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace remix::forward::mixed {
  bool bakedLightMasksEnabled() {
    static const bool enabled = std::getenv("REMIX_DEBUG_DISABLE_BAKED_LIGHT_MASKS") == nullptr;
    return enabled;
  }

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
          VkExtent2D{result.lookup.width, result.lookup.height}, VK_FORMAT_R32_UINT, result.lookup.layers);
      if (result.pages)
        result.pageImage = std::make_unique<OcclusionMap>(physical, device,
            VkExtent2D{result.pages->size, result.pages->size}, VK_FORMAT_R8_UNORM, result.pages->layers);
    }
    // These words exist only in the uploaded lookup. Cache and bake tables
    // retain the original format and the sparse records remain authoritative.
    void appendDirectionalOwnership(std::vector<uint32_t> &tail, size_t first,
                                    std::span<const uint32_t> directional) {
      if (directional.empty() ||
          (!bakedLightMasksEnabled() && !std::getenv("REMIX_DEBUG_DIRECTIONAL_OWNERSHIP_MASK"))) return;
      const uint32_t charts = directional[2], lights = directional[3];
      const uint32_t stride = uint32_t((uint64_t(lights) + 31u) / 32u);
      constexpr size_t maximumWords = (8u << 20) / sizeof(uint32_t);
      const uint64_t words = uint64_t(charts) * stride;
      if (words == 0 || words > maximumWords || words > UINT32_MAX - tail.size()) return;
      const size_t mask = tail.size();
      tail.resize(mask + size_t(words), 0);
      tail[first + 9] = uint32_t(directional.size());
      tail[first + 10] = stride;
      const uint32_t offsets = directional[5], entries = directional[6];
      for (uint32_t chart = 0; chart < charts; ++chart) {
        const size_t row = mask + size_t(chart) * stride;
        for (uint32_t entry = directional[offsets + chart]; entry < directional[offsets + chart + 1u]; ++entry) {
          const uint32_t light = directional[entries + size_t(entry) * DirectionalFields::entryWords];
          tail[row + light / 32u] |= 1u << (light % 32u);
        }
      }
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
        const size_t header = tail.size();
        tail.insert(tail.end(), directional.begin(), directional.end());
        if (first <= UINT32_MAX - directional.size() - ((8u << 20) / sizeof(uint32_t)))
          appendDirectionalOwnership(tail, header, directional);
        return directional.empty() ? 0u : offset;
      };
      result.directionalOffset = appendDirectional(words);
      result.lookup = plan.occlusion.pixels(limits.maxImageArrayLayers, words);
      result.compressedAllocation = imageAllocation(device, {result.lookup.width, result.lookup.height},
                                                    VK_FORMAT_R32_UINT, result.lookup.layers);
      uint64_t lookupAllocation = result.compressedAllocation, pageAllocation = 0;
      if (allowPages) result.pages = selectMaskPages(masks, charts, limits.maxImageDimension2D,
                                                     limits.maxImageArrayLayers, {}, budget);
      if (result.pages) {
        auto pageWords = result.pages->words(charts);
        indexMaskRecords(pageWords, charts);
        const auto directionalOffset = appendDirectional(pageWords);
        auto lookup = plan.occlusion.pixels(limits.maxImageArrayLayers, pageWords);
        const auto lookupBytes = imageAllocation(device, {lookup.width, lookup.height}, VK_FORMAT_R32_UINT, lookup.layers);
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
      const Plan &plan, const lightmap::Layout &layout,
      std::span<const uint16_t> pixels,
      bool allowPages, const UnifiedMasks *unified, std::span<const uint32_t> directional,
      bool preferCompleteDirectionals) {
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
        // A complete capture can contain owners absent from the four-channel
        // atlas. Compare memory within equivalent coverage; a smaller legacy
        // allocation must not silently discard those additional baked fields.
        if (preferCompleteDirectionals && legacy.indexedDirect && !directIndexed) continue;
        if (next.totalAllocation >= legacy.totalAllocation &&
            !(preferCompleteDirectionals && directIndexed && !legacy.indexedDirect)) continue;
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
    if (legacy.directionalOffset != 0u && legacy.lookup.words[legacy.directionalOffset + 9u] != 0u)
      std::fprintf(stderr, "Forward directional ownership mask: %u words per chart\n",
                   legacy.lookup.words[legacy.directionalOffset + 10u]);
    return legacy;
  }
}
