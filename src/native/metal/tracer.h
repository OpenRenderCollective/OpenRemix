#pragma once
#include "metal/raster.h"
#include "metal/statistics.h"
#include "pathtrace/options.h"
#include "pathtrace/path_inspection.h"
#include "scene/display.h"
#include "runtime/settings.h"
#include "runtime/sky_control.h"
#include "scene/trace_frame.h"
#include <remix/remix_native_ext.h>
#include <memory>
#include <string>
#include <vector>

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
      // Split local lighting (pathtrace/temporal.cpp): torch and emissive
      // families in histories of their own; splitMode its setting (3: one
      // combined direct pass).
      bool split = false, restir = false; // restir: ReSTIR DI's passes (pathtrace/restir.cpp)
      uint32_t splitMode = 0;
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
    // The sun and sky effects drawn over the frame are lit by
    // (pathtrace/renderer.h overlayLighting), for particles: the environment
    // and cloud shadow (id<MTLTexture>, with their id<MTLSamplerState>) and
    // the scene's TLAS (id<MTLAccelerationStructure>) with the resources its
    // instances reference, valid until the next prepare().
    struct OverlayLighting {
      float sunDirection[3] = {0, 0, -1};
      float sunRadiance[3] = {};
      float skyRadiance[4] = {};
      void *environment = nullptr, *environmentSampler = nullptr;
      bool environmentSrgb = false; // its texels are sRGB-encoded: decoded with pow 2.2
      float domeTransform[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
      void *cloudShadow = nullptr, *cloudShadowSampler = nullptr;
      float cloudShadowTransform[3][4] = {};
      uint32_t cloudShadowSlices = 0;
      uint32_t upAxis = 2;
      void *scene = nullptr;
      std::vector<void *> referenced;
    };
    OverlayLighting overlayLighting() const;
    // The path inspector (pathtrace/renderer.h): the pixel whose paths the
    // next frames record (x < 0: none), and the last frame's records.
    void inspectPixel(int x, int y);
    // remixapi_GetLocalLightSetStatisticsEXT (pathtrace/renderer.h).
    remixapi_LocalLightSetStatisticsEXT localLightSetStatistics() const;
    // remixapi_GetCameraCullingStatisticsEXT (pathtrace/renderer.h).
    remixapi_CameraCullingStatisticsEXT cameraCullingStatistics() const;
    PathInspection pathInspection() const;
    // HDR10 output's encoding of the composite (pathtrace/renderer.h setDisplayOutput).
    void setDisplayOutput(const scene::DisplayOutput &output);
    // The NUMOS sky (pathtrace/renderer.h setSky): the profile's sky settings,
    // the game's control over them (remix_sky_ext.h), and why the device
    // cannot run it (empty when it can).
    void setSky(const config::SkySettings &settings, const config::SkyControl &control,
                const std::string &unavailable);
    struct SkyStatus {
      bool supported = false, active = false;
      std::string reason; // why the sky is not active, or empty
    };
    SkyStatus skyStatus() const;
    // What the sky did in the last recorded frame; any thread.
    config::SkyState skyState() const;
    // The reconstruction (pathtrace/renderer.h upscalingStatistics): MetalFX
    // in the upscaler's and Ray Reconstruction's places.
    ReconstructionStatistics upscalingStatistics() const;
    // pathtrace/renderer.h: Russian roulette's pipeline variant is always
    // available on Metal; the translucent-surface filter ran this frame.
    bool adaptiveTerminationAvailable() const { return true; }
    bool translucencyDenoiserActive() const;
    // Frame interpolation inputs at render size (temporal configurations):
    // The frame's linear view depth (an id<MTLTexture>, R32 float, render
    // size) for host passes (REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT); null
    // without a denoiser.
    void *viewDepth() const;
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
