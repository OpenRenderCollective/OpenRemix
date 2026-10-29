#pragma once
// The FSR implementations behind FsrUpscaler and FsrFrameGeneration (fsr.cpp
// chooses one): the linked FSR 3.1.4 (fsr_linked.cpp,
// fsr_frame_generation.cpp; REMIX_FSR_LINKED) and AMD's ffx-api DLLs
// (fsr_ffx.cpp, fsr_frame_generation_ffx.cpp; REMIX_FSR_FFX).
#include "upscale/fsr.h"
#include "upscale/fsr_frame_generation.h"

namespace remix::upscale {
struct FsrUpscaler::Impl {
  virtual ~Impl() = default;
  virtual void create(VkPhysicalDevice physical, VkDevice device, VkExtent2D render, VkExtent2D display) = 0;
  virtual void dispatch(VkCommandBuffer command, const FsrDispatch &frame) = 0;
  virtual uint64_t memoryBytes() const = 0;
  virtual bool sharedImages() const = 0;
  std::string provider;
};
struct FsrFrameGeneration::Impl {
  virtual ~Impl() = default;
  virtual void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D display, VkFormat format,
                          VkExtent2D render, bool depthInverted) = 0;
  virtual void discontinuity() = 0;
  virtual void record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless,
                      Input output, const FrameGenerationCamera &camera, float frameMilliseconds) = 0;
  std::string provider;
};
#if REMIX_FSR_LINKED
std::unique_ptr<FsrUpscaler::Impl> linkedUpscaler();
std::unique_ptr<FsrFrameGeneration::Impl> linkedFrameGeneration();
#endif
#if REMIX_FSR_FFX
std::unique_ptr<FsrUpscaler::Impl> ffxUpscaler();
std::unique_ptr<FsrFrameGeneration::Impl> ffxFrameGeneration();
#endif
} // namespace remix::upscale
