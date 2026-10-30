#pragma once
#include "upscale/renderer.h"
#include <memory>
#include <string>

namespace remix::upscale {
// The FSR upscaler behind Renderer. Linux links FSR 3.1.4 from
// third_party/fsr31 (fsr_linked.cpp). Windows loads AMD's signed ffx-api DLLs
// from FidelityFX SDK 2.4.0 (fsr_ffx.cpp), whose loader picks the newest
// provider the GPU and driver support (FSR 4 on RDNA 4, else FSR 3.1).
//
// Test switches on Windows (fsr.cpp): REMIX_FSR_VERSION=3 asks the DLLs for
// their FSR 3.1 provider; REMIX_FSR_LINKED=1 runs the linked FSR 3.1.4 in
// builds made with -Dfsr_linked=true.
struct FsrDispatch {
  // Inputs in GENERAL: color (HDR, render size), device depth (reversed),
  // unjittered pixel motion, reactive and composition masks; output (display
  // size) in GENERAL, left so.
  Input color, depth, motion, reactive, composition, output;
  // The linked SDK's caller-owned intermediates (sharedImages()).
  Input dilatedDepth, dilatedMotion, previousDepth;
  VkExtent2D render{}, display{};
  float jitter[2]{}; // as the SDK takes it: the negated ray jitter
  float sharpness{}, milliseconds{};
  // The distances FSR names after device depth 0 ("near") and 1 ("far").
  float cameraNear{}, cameraFar{}, fieldOfView{};
  bool reset{};
};
class FsrUpscaler {
public:
  FsrUpscaler();
  ~FsrUpscaler();
  // Throws without leaving anything behind.
  void create(VkPhysicalDevice physical, VkDevice device, VkExtent2D render, VkExtent2D display);
  void dispatch(VkCommandBuffer command, const FsrDispatch &frame);
  uint64_t memoryBytes() const;
  // Whether dispatch() takes FsrDispatch's dilated and previous depth images.
  bool sharedImages() const;
  // The provider evaluating, e.g. "FSR 4.1.2"; empty before create().
  const std::string &provider() const;

  // One implementation (fsr_backends.h).
  struct Impl;

private:
  std::unique_ptr<Impl> m;
};
} // namespace remix::upscale
