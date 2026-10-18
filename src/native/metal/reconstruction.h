#pragma once
// Objective-C++ only: reconstruction with MetalFX (upscale/renderer.h's
// counterpart). MetalFX's temporal scaler takes FSR 3.1 / DLSS-SR's place and
// its temporal denoised scaler DLSS Ray Reconstruction's: the tracer's inputs,
// the input preparation and the display composite are the Vulkan renderer's.
#include "metal/gpu.h"
#include "scene/types.h"
#include <string>

namespace remix::metal::reconstruction {
  // remixapi_ReconstructionStatisticsEXT::activeBackend reports these.
  enum class Backend : uint32_t { Native, MetalFXUpscaling, MetalFXDenoising };
  const char *name(Backend backend);
  // Availability on this device and OS (empty reason when supported).
  bool upscalingSupported(id<MTLDevice> device, std::string &reason);
  bool denoisingSupported(id<MTLDevice> device, std::string &reason);
  // Render size of a quality preset (native AA, quality, balanced,
  // performance, ultra performance: FSR's ratios), within MetalFX's limits.
  void renderSize(id<MTLDevice> device, Backend backend, uint32_t quality, uint32_t displayWidth,
                  uint32_t displayHeight, uint32_t &width, uint32_t &height);
  // Sub-pixel jitter in render pixels (upscale/renderer.cpp jitter: Halton
  // 2/3 over 8 * scale^2 phases).
  void jitter(float *xy, uint64_t frame, uint32_t renderWidth, uint32_t displayWidth);
  // upscale/renderer.cpp supportsCamera: a finite perspective projection.
  bool supportsCamera(const scene::Matrix &projection);

  struct Guides {
    id<MTLTexture> diffuse = nil, specular = nil, normalRoughness = nil, hitDistance = nil;
  };
  struct Statistics {
    Backend backend = Backend::Native;
    uint32_t renderWidth = 0, renderHeight = 0, displayWidth = 0, displayHeight = 0;
    uint64_t frames = 0, resets = 0, memoryBytes = 0;
  };

  class Renderer {
  public:
    Renderer();
    ~Renderer();
    void initialize(id<MTLDevice> device, Backend backend, uint32_t renderWidth, uint32_t renderHeight,
                    uint32_t displayWidth, uint32_t displayHeight, MTLPixelFormat displayFormat);
    // Prepares device depth, motion and the reactive mask from the
    // denoiser's guides, then encodes MetalFX into command.
    void record(id<MTLCommandBuffer> command, id<MTLTexture> color, id<MTLTexture> viewDepth, id<MTLTexture> motion,
                id<MTLTexture> transmission, const scene::Matrix &projection, const scene::Matrix &view,
                const float *jitter, bool reset, const Guides &guides = {});
    // The display composite (upscale.frag): post-processing, display encoding
    // and the stable depth for raster overlays.
    void composite(id<MTLRenderCommandEncoder> encoder, id<MTLTexture> stableDepth, bool encodeOutput, bool prism,
                   bool gamma22, bool hardwareSrgb, id<MTLBuffer> exposure, id<MTLTexture> bloom,
                   id<MTLSamplerState> bloomSampler, id<MTLBuffer> postSettings, uint32_t slot);
    id<MTLTexture> output() const;
    // Render-resolution inputs frame interpolation shares.
    id<MTLTexture> deviceDepth() const;
    id<MTLTexture> motion() const;
    // The temporal scaler, when frame interpolation can take it as its scaler.
    id scaler() const;
    Statistics statistics() const;

    struct Gpu;

  private:
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::metal::reconstruction
