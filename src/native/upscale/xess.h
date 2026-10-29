#pragma once
#include "upscale/renderer.h"
#include <memory>
#include <string>
#include <vector>

namespace remix::upscale {
// XeSS Super Resolution behind Renderer: Windows only, like FSR's ffx-api DLLs.
// xess.cpp loads Intel's signed libxess.dll (XeSS SDK 3.0.2, the submodule
// third_party/xess/xess) from beside remix.dll: its xess/ or upscale/ folder,
// or its own. Elsewhere XeSS reports itself unavailable.
namespace xess {
#if REMIX_HAS_XESS
// The Vulkan instance version libxess needs (it asks for no instance
// extensions); 0 where it is not packaged.
uint32_t instanceApiVersion();
// Adds libxess's device extensions to `extensions` and its features to the
// chain `features` heads (a VkPhysicalDeviceFeatures2 first), which
// vkCreateDevice then takes as its pNext: libxess sets fields of the chain's
// structures and links its own. Returns why XeSS is unavailable on this device
// (nothing is added), or empty. The names and structures added are libxess's.
std::string enableDevice(VkInstance instance, VkPhysicalDevice physical,
                         uint32_t instanceApiVersion,
                         std::vector<const char *> &extensions,
                         void *&features);
// The outcome of enableDevice for a created device, until it is destroyed.
void registerDevice(VkDevice device, const std::string &unavailable);
void unregisterDevice(VkDevice device);
#else
inline uint32_t instanceApiVersion() { return 0; }
inline std::string enableDevice(VkInstance, VkPhysicalDevice, uint32_t,
                                std::vector<const char *> &, void *&) {
  return "XeSS is Windows only";
}
inline void registerDevice(VkDevice, const std::string &) {}
inline void unregisterDevice(VkDevice) {}
#endif
} // namespace xess

#if REMIX_HAS_XESS
struct XessDispatch {
  // Inputs in SHADER_READ_ONLY_OPTIMAL: color (HDR, render size), device depth
  // (not reversed), unjittered pixel motion from this frame to the previous;
  // output (display size) in GENERAL.
  Input color, depth, motion, output;
  VkExtent2D render{};
  float jitter[2]{}; // as XeSS takes it: the negated ray jitter
  bool reset{};
};
class XessUpscaler {
public:
  XessUpscaler();
  ~XessUpscaler();
  // `quality`: the preset index of xessRenderSize. Builds XeSS's pipelines,
  // which takes no queue: it may run on a preparation worker. Throws without
  // leaving anything behind.
  void create(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
              VkExtent2D render, VkExtent2D display, uint32_t quality);
  // xessVKInit, which uploads XeSS's weights through the device's queue
  // itself: only where the queue may be used (under the runtime's queue
  // mutex), as NgxFeature::finishInitialization. Before any dispatch; a
  // failure is reported by dispatch().
  void finishInitialization();
  void dispatch(VkCommandBuffer command, const XessDispatch &frame);
  uint64_t memoryBytes() const;
  // E.g. "XeSS 2.0.2"; empty before create().
  const std::string &provider() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
#endif
} // namespace remix::upscale
