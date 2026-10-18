#pragma once
// Objective-C++ only: GPU pass timings on Metal ("rtx.native.gpuPassTimers";
// platform/gpu_timers.h's counterpart). A timed frame samples GPU timestamps
// at the start and end of every encoder made through the helpers below
// (counter sample buffers at stage boundaries); the frame slot's samples are
// resolved into gpu::FrameTimings once its work has completed.
#import <Metal/Metal.h>
#include "platform/gpu_frames.h"
#include "platform/gpu_timings.h"
#include <vector>

namespace remix::metal {
  class PassTimers {
  public:
    // Whether the device samples timestamps at encoder boundaries.
    bool initialize(id<MTLDevice> device);
    bool supported() const { return m_supported; }
    // At a frame slot's reuse: the timings of the frame it last held, when
    // it was timed. Then starts timing this frame when enabled.
    void beginFrame(uint32_t slot, bool enabled, uint64_t frame);
    // The encoders made from here on are not timed (the frame is recorded).
    void endFrame();
    const gpu::FrameTimings &latest() const { return m_latest; }
    // The recorded frame's render and output sizes.
    void setSizes(uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight);

    // The timers of the frame being recorded, or null.
    static PassTimers *active();
    // A range of samples (start, end) for an encoder, or false when untimed.
    bool reserve(const char *name, uint32_t width, uint32_t height, NSUInteger &start, NSUInteger &end);
    id<MTLCounterSampleBuffer> buffer() const { return m_slots[m_slot].samples; }

  private:
    struct Slot {
      id<MTLCounterSampleBuffer> samples = nil;
      std::vector<gpu::PassRange> ranges;
      uint64_t frame = 0;
      uint32_t sizes[4] = {}; // render and output width, height
      bool pending = false;
    };
    void resolve(Slot &slot);
    id<MTLDevice> m_device = nil;
    bool m_supported = false, m_recording = false;
    Slot m_slots[gpuFrameCount];
    uint32_t m_slot = 0;
    gpu::FrameTimings m_latest;
    // CPU/GPU timestamp calibration (nanoseconds per GPU tick).
    MTLTimestamp m_cpuStart = 0, m_gpuStart = 0;
  };

  // Encoders that are timed when a frame records timings (label also names the pass).
  id<MTLComputeCommandEncoder> computeEncoder(id<MTLCommandBuffer> command, NSString *label, uint32_t width = 0,
                                              uint32_t height = 0);
  id<MTLBlitCommandEncoder> blitEncoder(id<MTLCommandBuffer> command, NSString *label);
  id<MTLRenderCommandEncoder> renderEncoder(id<MTLCommandBuffer> command, MTLRenderPassDescriptor *pass,
                                            NSString *label, uint32_t width = 0, uint32_t height = 0);
  id<MTLAccelerationStructureCommandEncoder> accelerationEncoder(id<MTLCommandBuffer> command, NSString *label);
  // Work encoded by a framework (MetalFX) is not timed itself: a timed
  // encoder of minimal work right after it ends a range covering it, since
  // a range starts where the timed work before it ended.
  void markTimed(id<MTLCommandBuffer> command, NSString *label);
} // namespace remix::metal
