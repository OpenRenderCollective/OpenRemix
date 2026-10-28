#pragma once
#include "upscale/renderer.h"
#include <memory>
#include <string>

namespace remix::upscale {
// FSR frame generation: one generated frame halfway between the previous and
// the current backbuffer. The counterpart of NgxFrameGeneration, on any GPU.
// Linux and macOS link FSR 3.1.4's optical flow and frame interpolation
// effects (fsr_frame_generation.cpp); Windows loads AMD's signed ffx-api DLLs
// (fsr_frame_generation_ffx.cpp), FSR 4's ML frame generation on RDNA 4.
//
// It takes depth and motion vectors directly (the prepare stage dilates
// them), not the FSR upscaler's shared resources, so it runs behind either
// upscaler. AMD's frame-pacing swapchain is not used: the runtime presents
// the frames. The implementation follows the upscaler's (fsr.h, fsr.cpp).
class FsrFrameGeneration {
public:
  FsrFrameGeneration();
  ~FsrFrameGeneration();
  // `format`: of the backbuffer, HUD-less and output images (8-bit RGBA or
  // BGRA holding the sRGB-encoded frame); `render`: of depth and motion;
  // `depthInverted`: of the depth record() is given, which selects a shader
  // permutation. Compiles the effects' pipelines: call on the pipeline
  // service's worker (runtime/pipelines.h). Throws without leaving anything
  // behind.
  void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D display, VkFormat format,
                  VkExtent2D render, bool depthInverted);
  // Nothing is deferred to the queue; present for the shape of NgxFrameGeneration.
  bool finishInitialization(VkQueue queue);
  // A rendered frame was shown without a record(): the kept previous frame is
  // not the one before the next record(), which therefore resets.
  void discontinuity();
  // `backbuffer`, `depth`, `motion` and `hudless` in SHADER_READ_ONLY_OPTIMAL,
  // `output` in GENERAL, all left so. `hudless`: the backbuffer before its
  // interface was drawn (no image: not provided). `motion`: in render pixels,
  // from a pixel of this frame to where it was in the previous one, without
  // jitter. `frameMilliseconds`: between the previous rendered frame and this one.
  void record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless, Input output,
              const FrameGenerationCamera &camera, float frameMilliseconds);
  // The provider generating, e.g. "FSR 4.0.2"; empty before initialize().
  const std::string &provider() const;

  // One implementation (fsr_backends.h).
  struct Impl;

private:
  std::unique_ptr<Impl> m;
};
} // namespace remix::upscale
