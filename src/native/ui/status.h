#pragma once
// The developer panel's view of the runtime. The tabs (ui/rendering.cpp,
// lighting.cpp, post_processing.cpp, diagnostics.cpp, textures.cpp) are shared
// by the Vulkan and Metal runtimes and ask what differs between them (device,
// swapchain or drawables, reconstruction, frame generation) through these
// queries, which each runtime answers (runtime/ui_status.cpp, metal/ui.mm).
#include "pathtrace/path_inspection.h"
#include "ui/sky.h"
#include "upscale/options.h"
#include <imgui.h>
#include <remix/remix_native_ext.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace remix::ui {
  // The ImGui renderer's textures, for ui/textures.cpp (ui/integration.cpp on
  // Vulkan, metal/ui.mm on Metal): updateTexture uploads or destroys a managed
  // texture now; addTexture shows a renderer image (RemixNativeRuntime::
  // uiTextureView: a VkImageView or an id<MTLTexture>) as an ImGui texture;
  // removeTexture releases it.
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
  // false: Off and one entry for every other mode, with neutral quality
  // preset names), the denoising reconstruction's checkbox
  // (dlssRayReconstruction) and whether it has DLSS Ray Reconstruction's
  // model and preset choices, FSR's sharpening, and frame generation's backend
  // combo (null: one generator) and frame counts.
  struct ReconstructionChoices {
    const char *upscalers = "Off\0Auto\0FSR\0DLSS\0";
    bool vendorModes = true;
    const char *denoiser = "DLSS Ray Reconstruction";
    bool denoiserModels = true;
    bool sharpening = true;
    const char *frameGenerationBackends = "Auto\0DLSS\0FSR\0";
    bool multiFrameGeneration = true;
    const char *frameGenerationTooltip =
        "Shows generated frames between each pair of rendered frames: DLSS Frame Generation\n"
        "(2x to 4x, a supported RTX GPU) or FSR frame generation (2x, any GPU).\n"
        "Needs the path tracer and an upscaler (DLSS or FSR; their native-resolution preset\n"
        "counts). Adds latency: the rendered frame is shown after its generated ones.";
  };

  // The reconstruction the last frame was rendered with (upscale::Statistics
  // on Vulkan, metal::ReconstructionStatistics on Metal).
  struct ReconstructionStatus {
    std::string name = "Native"; // the backend, or the provider that ran it
    bool active = false;         // an upscaler or denoising reconstruction ran (TAA stands down)
    bool denoises = false;       // it replaces the denoisers (Ray Reconstruction, MetalFX denoising)
    // DLSS Ray Reconstruction's model (CNN or transformer) and preset (1-6: A-F).
    bool models = false, cnn = false;
    uint32_t preset = 0;
    std::string note; // why the requested model or preset is not the one running
    uint32_t renderWidth = 0, renderHeight = 0, displayWidth = 0, displayHeight = 0;
    float milliseconds = 0;
    std::string failure;
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

  // HDR10 output: whether its settings apply (the display has HDR on and the
  // backend can output it), and if so whether it is active or why not.
  struct HdrStatus {
    bool available = false, active = false;
    std::string inactive;
  };

  // Screenshot sets (the panel's button and shortcut, PNG and HDR files in
  // the profiles directory's screenshots folder), and the last one's outcome.
  struct ScreenshotStatus {
    bool available = false;
    std::string status;
  };

  // The path inspector (Diagnostics): the last inspected pixel's paths, and
  // the render size a click on the scene maps to. available: the backend
  // records paths (the Vulkan path tracer).
  struct PathInspectionStatus {
    bool available = false;
    uint32_t renderWidth = 0, renderHeight = 0;
    PathInspection inspection;
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
