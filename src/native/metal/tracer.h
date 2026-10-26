#pragma once
#include "metal/raster.h"
#include "metal/ui/status.h"
#include "pathtrace/options.h"
#include "scene/trace_frame.h"
#include <remix/remix_native_ext.h>
#include <memory>
#include <string>

namespace remix::metal {
  // The Metal path tracer (pathtrace/renderer.h's counterpart): Metal
  // acceleration structures and the path tracing shaders translated from the
  // same SPIR-V. GPU state is private to metal/tracer.mm.
  class PathTracer {
  public:
    static constexpr uint32_t textureLimit = 4096;
    // The resources and pipelines a tracer is prepared with (the Vulkan
    // runtime's resource key): changing any prepares a new tracer.
    struct Configuration {
      // Render (tracing) and display (output) sizes: they differ with upscaling.
      // The display is UNORM: the composite writes gamma 2.2 values.
      uint32_t width = 0, height = 0, displayWidth = 0, displayHeight = 0, displayFormat = 0;
      bool reblur = false, taa = false, sigma = false, local = false, localDenoise = false;
      bool sharc = false, validation = false;
      uint32_t sharcCapacityLog2 = 0;
      // reconstruction::Backend: native, MetalFX upscaling or MetalFX denoising
      // (which replaces NRD and TAA, as DLSS Ray Reconstruction does).
      uint32_t reconstruction = 0;
      std::string reconstructionUnavailable; // why a requested MetalFX mode is off
      // MetalFX frame interpolation (needs the temporal path's depth and motion).
      bool frameGeneration = false;
      // The lighting pipelines' base constants (pathtrace/sharc.cpp
      // createLightingPipelines): rtx.native.pathtrace.shadowEarlyOut, and
      // the separate indirect pass's consumer (indirectPlanned).
      bool earlyOut = false, separateIndirect = false;
      bool operator==(const Configuration &) const = default;
      // pathtrace/temporal.cpp beginFrame's decisions; local: local lights
      // were seen (the runtime's sticky local-source contract); perspective:
      // the world camera supports reconstruction. device: an id<MTLDevice>.
      static Configuration of(void *device, const TraceOptions &options, bool frameGeneration, bool local,
                              bool perspective, uint32_t displayWidth, uint32_t displayHeight,
                              uint32_t displayFormat);
      bool temporal() const { return reblur || taa || sigma || reconstruction; }
    };
    PathTracer();
    ~PathTracer();
    // device: an id<MTLDevice>. Compiles the configuration's pipelines
    // (seconds on first use; may run on any thread).
    void initialize(void *device, const Configuration &configuration);
    const Configuration &configuration() const;
    void selectFrame(uint32_t slot);
    // CPU preparation: may run while the previous frame is on the GPU.
    scene::TraceFrame prepareScene(Raster &resources, const TraceOptions &options) const;
    // Encodes the frame's uploads, acceleration structure builds and tracing
    // passes after command (an id<MTLCommandBuffer>) on its queue.
    void prepare(void *command, Raster &resources, const TraceOptions &options, scene::TraceFrame &scene);
    // Composites into the display pass (an id<MTLRenderCommandEncoder>).
    void record(void *encoder);
    remixapi_TracingStatisticsEXT statistics() const;
    remixapi_FramePerformanceEXT performance() const;
    remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    remixapi_SharcStatisticsEXT sharcStatistics() const;
    remixapi_LocalLightStatisticsEXT localLightStatistics() const;
    remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
    // Automatic exposure and bloom (postfx Statistics' fields).
    struct PostStatistics {
      float exposure = 1, target = 1, limit = 0, milliseconds = 0;
      uint64_t memoryBytes = 0;
    };
    PostStatistics postStatistics() const;
    // The reconstruction (pathtrace/renderer.h upscalingStatistics): MetalFX
    // in the upscaler's and Ray Reconstruction's places.
    ReconstructionStatistics upscalingStatistics() const;
    // pathtrace/renderer.h: Russian roulette's pipeline variant is always
    // available on Metal; the translucent-surface filter ran this frame.
    bool adaptiveTerminationAvailable() const { return true; }
    bool translucencyDenoiserActive() const;
    // Frame interpolation inputs at render size (temporal configurations):
    // id<MTLTexture> device depth and pixel motion, and the temporal scaler.
    void *interpolationDepth() const;
    void *interpolationMotion() const;
    void *interpolationScaler() const;
    // The frame's camera for interpolation: projection (Vulkan framebuffer
    // Y), view, jitter in render pixels and whether history was reset.
    void interpolationCamera(scene::Matrix &projection, scene::Matrix &view, float *jitter, bool &reset) const;
    void resetPerformance();

    struct Gpu;

  private:
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::metal
