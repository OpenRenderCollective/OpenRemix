#pragma once
// Objective-C++ only: the Metal denoiser (denoise/renderer.h's counterpart):
// the temporal guides, NRD REBLUR/SIGMA passes, confidence filter, composition
// and TAA, with the NRD library's dispatches on Metal.
#include "metal/frame.h"
#include "metal/gpu.h"
#include "scene/types.h"
#include <remix/remix_native_ext.h>
#include <array>
#include <memory>

namespace remix::metal::denoise {
  // The image slots (set 1 binding i, i + 4 from Penumbra on), the frame
  // constants and local tuning: metal/frame.h.

  class Renderer {
  public:
    Renderer();
    ~Renderer();
    // As denoise::Renderer::initialize (no split local lighting on Metal).
    // rayReconstruction: the MetalFX denoiser's guides (RR images) at full size.
    void initialize(id<MTLDevice> device, uint32_t width, uint32_t height, bool reblur, bool sigma, bool local,
                    bool localDenoise, bool validation, bool localReconstruction, bool foregroundDenoise,
                    bool rayReconstruction = false);
    void selectFrame(uint32_t slot);
    id<MTLTexture> image(ImageSlot slot) const;
    // Writes set 1 (the temporal inputs) of a tracing or denoising stage.
    void bindInputs(ArgumentSet &set) const;
    void begin(id<MTLCommandBuffer> command, const Frame &frame, const scene::Matrix &projection,
               const scene::Matrix &previousProjection, id<MTLTexture> world, id<MTLTexture> sky,
               bool localFireflyFilter = false);
    void setLocalTuning(const LocalTuning &tuning);
    void recordOpaque(id<MTLCommandBuffer> command);
    void record(id<MTLCommandBuffer> command);
    void submitted();
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    remixapi_DenoisingStatisticsEXT statistics() const;
    bool foregroundActive() const;

    struct Gpu;

  private:
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::metal::denoise
