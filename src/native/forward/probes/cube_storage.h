#pragma once
#include "forward/probes/data.h"
#include <vulkan/vulkan.h>
#include <cstdlib>

namespace remix::forward::probes {
  // All consumers of a published probe image use the same storage for the
  // lifetime of the device, including fallback images and composition copies.
  inline bool nativeCubeStorage(VkPhysicalDevice physical) {
    static const bool requested = std::getenv("REMIX_DEBUG_NATIVE_PROBE_CUBE") != nullptr;
    if (!requested) return false;
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical, &features);
    if (!features.imageCubeArray) return false;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    return properties.limits.maxImageArrayLayers >= 6 * maxProbes;
  }
}
