#pragma once
// The Metal developer UI's view of the runtime. The tabs in metal/ui/ are the
// Vulkan renderer's (ui/rendering.cpp, lighting.cpp, post_processing.cpp,
// diagnostics.cpp, textures.cpp) with what is Vulkan's (device properties,
// swapchain, NGX, FSR and DLSS) read through these queries, which
// metal/ui.mm answers. Port upstream changes to those files here.
#include "upscale/options.h"
#include <imgui.h>
#include <remix/remix_native_ext.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace remix::metal {
  // The reconstruction a frame was rendered with. Values are the public
  // numbering (remix_reconstruction_ext.h): MetalFX upscaling in the
  // upscaler's slot, MetalFX denoising in Ray Reconstruction's.
  enum class ReconstructionBackend : uint32_t { Native = 0, MetalFxUpscaling = 1, MetalFxDenoising = 3 };
  inline const char *name(ReconstructionBackend backend) {
    switch (backend) {
    case ReconstructionBackend::MetalFxUpscaling: return "MetalFX upscaling";
    case ReconstructionBackend::MetalFxDenoising: return "MetalFX denoising";
    default: return "Native";
    }
  }
  inline bool denoises(ReconstructionBackend backend) { return backend == ReconstructionBackend::MetalFxDenoising; }
  struct Extent {
    uint32_t width = 0, height = 0;
  };
  // upscale::Statistics' fields (upscale/renderer.h).
  struct ReconstructionStatistics {
    ReconstructionBackend backend{};
    upscale::Mode requested{};
    bool srSupported{}, rrSupported{}, sdkAvailable{}, failed{};
    uint64_t failures{}, memoryBytes{};
    bool active{};
    Extent render{}, display{};
    uint64_t frames{}, resets{};
    float milliseconds{};
    std::string failure;
  };
} // namespace remix::metal

namespace remix::ui {
  // The Metal ImGui renderer's textures (metal/ui.mm), for metal/ui/textures.cpp:
  // updateTexture uploads or destroys a managed texture now; addTexture shows
  // an id<MTLTexture> as an ImGui texture; removeTexture releases it.
  void updateTexture(ImTextureData *texture);
  ImTextureID addTexture(const void *view);
  void removeTexture(ImTextureID texture);

  struct DeviceStatus {
    std::string name; // the GPU
    std::string api;  // the GPU API and its version
  };

  // Pipeline preparation (the Vulkan pipeline service, Metal's compiler).
  struct PreparationStatus {
    bool available = false;
    uint64_t completed = 0, requested = 0, created = 0, reused = 0, failed = 0, deduplicated = 0, cacheHits = 0;
    double milliseconds = 0, driverMilliseconds = 0;
    bool cacheLoaded = false, feedback = false, cacheWriteFailed = false;
    // A configuration being prepared in the background, and for how long.
    bool pending = false;
    uint64_t generation = 0;
    double elapsedSeconds = 0;
    std::string failure;
  };

  struct FrameGenerationStatus {
    std::string status;      // what generates (empty: applies at the next frame)
    std::string unavailable; // why the requested generation cannot run
    uint64_t generated = 0, duplicated = 0, ungenerated = 0;
    double renderedInterval = 0; // smoothed milliseconds between rendered frames
    size_t presentImages = 0;    // swapchain images / drawables
    const char *presentMode = "";
    bool lowLatencySupported = false;
    const char *lowLatencyUnavailable = ""; // why low latency is unavailable
  };

  // The reconstruction features the backend offers, as the Rendering tab
  // labels them: the upscaler combo (indexed by upscale::Mode; vendorModes
  // false: Off and one entry for every other mode), the denoising
  // reconstruction's checkbox (dlssRayReconstruction), FSR's sharpening, and
  // frame generation's backend combo (null: one generator) and frame counts.
  struct ReconstructionChoices {
    const char *upscalers = "Off\0Auto\0FSR 3.1\0DLSS\0";
    bool vendorModes = true;
    const char *denoiser = "DLSS Ray Reconstruction";
    bool sharpening = true;
    const char *frameGenerationBackends = "Auto\0DLSS\0FSR 3.1\0";
    bool multiFrameGeneration = true;
    const char *frameGenerationTooltip =
        "Shows generated frames between each pair of rendered frames: DLSS Frame Generation\n"
        "(2x to 4x, a supported RTX GPU) or FSR 3.1 frame generation (2x, any GPU).\n"
        "Needs the path tracer and an upscaler (DLSS or FSR; their native-resolution preset\n"
        "counts). Adds latency: the rendered frame is shown after its generated ones.";
  };

  // Automatic exposure and bloom (postfx).
  struct PostStatus {
    bool available = true; // the backend has the exposure and bloom passes
    float exposure = 1, target = 0;
    int limit = 0; // -1 at the minimum EV limit, 1 at the maximum
    float milliseconds = 0;
    uint64_t memoryBytes = 0;
    float activeExposure = 1; // exposure applied to the frame
    std::string failure;
  };

  // Asynchronous presentation, one side of a Present (runtime.h AsyncPresentStatistics).
  struct AsyncPresent {
    double waitMilliseconds = 0, setupMilliseconds = 0, captureMilliseconds = 0;
    double uiMilliseconds = 0, callMilliseconds = 0, renderMilliseconds = 0;
    uint64_t handedOff = 0, rendered = 0;
  };

  // The persistent static set (pathtrace/renderer.h StaticStatistics).
  struct StaticSet {
    bool active = false;
    uint64_t units = 0, surfaces = 0, reusedSurfaces = 0, rebuiltSurfaces = 0, dynamicSurfaces = 0;
    uint64_t rebuiltUnits = 0, changes = 0, temporalSurfaces = 0, relayouts = 0;
    double syncMilliseconds = 0, tableMilliseconds = 0;
    uint64_t validations = 0, mismatches = 0;
    std::string mismatch;
  };

  // The previous Present's CPU costs.
  struct PresentStatus {
    remixapi_FramePerformanceEXT performance{.structSize = sizeof(remixapi_FramePerformanceEXT)};
    remixapi_FrameBreakdownEXT breakdown{.structSize = sizeof(remixapi_FrameBreakdownEXT)};
    bool async = false;
    AsyncPresent caller, current, render; // previous caller Present, this one, the render thread
    StaticSet statics;
  };
} // namespace remix::ui
