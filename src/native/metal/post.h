#pragma once
// Objective-C++ only: automatic exposure and bloom on Metal (postfx/renderer.h's
// counterpart), from the same shaders (postfx.comp: meter, resolve, down, up).
// The composite (upscale.frag / the path tracer's) reads exposure(), bloom()
// and settings() as postfx's composite set.
#include "metal/gpu.h"
#include "pathtrace/options.h"
#include "scene/temporal.h"
#include <array>
#include <chrono>
#include <memory>
#include <vector>

namespace remix::metal::post {
  struct Statistics {
    float exposure = 1, target = 1, limit = 0, milliseconds = 0;
    uint64_t memoryBytes = 0, meterFrames = 0, bloomFrames = 0;
    bool adapting = false;
  };

  class Renderer {
  public:
    Renderer();
    ~Renderer();
    // display: the composited image's size (the HDR input of record);
    // render: the traced size (the meter's material guide).
    void initialize(id<MTLDevice> device, uint32_t displayWidth, uint32_t displayHeight, uint32_t renderWidth,
                    uint32_t renderHeight);
    // At a frame slot's reuse (its previous frame has completed): reads the
    // exposure that frame resolved.
    void selectFrame(uint32_t slot);
    // The tracer's material guide (pathtrace.comp meterMaterial), at render size.
    id<MTLTexture> guide() const;
    // postfx Renderer::record: meter and resolve the exposure, then the bloom
    // pyramid, from hdr (display size). changed: the traced image changed;
    // bypass: debug views keep a unity exposure and no bloom.
    void record(id<MTLCommandBuffer> command, id<MTLTexture> hdr, const TraceOptions &options,
                scene::TemporalEpoch epoch, bool changed, bool bypass, uint64_t generation);
    id<MTLBuffer> exposure() const;
    id<MTLTexture> bloom() const;
    id<MTLBuffer> settings() const;
    Statistics statistics() const;

    struct Gpu;

  private:
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::metal::post
