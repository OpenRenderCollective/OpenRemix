#pragma once
// Objective-C++ only: the Metal runtime's device, presentation and UI state.
#include "metal/raster_gpu.h"
#include "metal/runtime.h"
#include "metal/tracer.h"
#include <chrono>
#include <dispatch/dispatch.h>

namespace remix {
  struct RemixNativeRuntime::Gpu {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer *layer = nil;
    // Frames in flight: a slot is reused after its command buffer completed.
    dispatch_semaphore_t frames = nullptr;
    uint32_t frameIndex = 0, frameCount = gpuFrameCount;
    uint32_t width = 0, height = 0;
    MTLPixelFormat format = MTLPixelFormatBGRA8Unorm;
    // A failure reported by a completed command buffer.
    std::atomic<bool> commandFailed{false};
    // Presented screenshot: the drawable is copied into a shared buffer by the
    // frame's command buffer and written out once it completes.
    struct Screenshot {
      std::string path;
      id<MTLBuffer> readback = nil;
      uint32_t width = 0, height = 0;
      bool bgra = true;
      id<MTLCommandBuffer> command = nil;
    } screenshot;
    // GPU pass timings (rtx.native.gpuPassTimers).
    metal::PassTimers timers;
    // MetalFX frame interpolation (macOS 26), as RTGL-D2-Metal: the scene
    // renders offscreen into a slot's color, the interpolator writes the frame
    // between the previous slot's color and this one into its output, and the
    // UI is drawn over the output and over real (a copy of color). Once the
    // frame's work completes, the present queue blits both into drawables and
    // presents them paced, so acquiring drawables never blocks recording.
    struct Interpolation {
      id interpolator = nil; // id<MTLFXFrameInterpolator>
      struct Slot {
        id<MTLTexture> color = nil, output = nil, real = nil;
      } slots[gpuFrameCount];
      uint32_t previous = UINT32_MAX; // the slot holding the previous frame's color
      id<MTLCommandQueue> presentQueue = nil;
      dispatch_queue_t presentDispatch = nullptr;
      // The configuration the interpolator was created for.
      uint32_t width = 0, height = 0, inputWidth = 0, inputHeight = 0;
      void *depth = nullptr;
      bool reset = true;
      std::string failure; // why interpolation is off for this configuration
      std::chrono::steady_clock::time_point last{};
      float interval = 0; // seconds between rendered frames
      uint64_t generated = 0;
    } interpolation;
    // Dear ImGui's draw data: the font atlas and per-slot vertex/index buffers.
    struct Ui {
      id<MTLRenderPipelineState> pipeline = nil;
      id<MTLDepthStencilState> depth = nil;
      id<MTLSamplerState> sampler = nil;
      std::unordered_map<uint64_t, id<MTLTexture>> textures;
      uint64_t nextTexture = 1;
      id<MTLBuffer> vertices[gpuFrameCount] = {}, indices[gpuFrameCount] = {};
      MTLPixelFormat format = MTLPixelFormatInvalid;
    } ui;
  };
} // namespace remix
