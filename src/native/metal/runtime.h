#pragma once
// The Metal runtime: the API's runtime in macOS builds
// (runtime/runtime.h in Vulkan builds). The public interface is the one the
// API layer uses; Metal state is private to metal/*.mm.
#include "metal/diagnostics.h"
#include "runtime/host_passes.h"
#include "metal/raster.h"
#include "metal/tracer.h"
#include "pathtrace/options.h"
#include "metal/statistics.h"
#include "ui/status.h"
#include "platform/gpu_timings.h"
#include "platform/render_thread.h"
#include "platform/window.h"
#include "runtime/profiles.h"
#include "runtime/screenshot.h"
#include "runtime/sky_control.h"
#include "scene/free_camera.h"
#include "scene/particles.h"
#include "ui/menu_shortcut.h"
#include "ui/profiles.h"
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>
#include <string>

struct ImGuiContext;

namespace remix {
  namespace metal {
    class PathTracer;
    struct PreparedTracer;
  } // namespace metal

  class RemixNativeRuntime {
  public:
    RemixNativeRuntime();
    ~RemixNativeRuntime();
    RemixNativeRuntime(const RemixNativeRuntime &) = delete;
    RemixNativeRuntime &operator=(const RemixNativeRuntime &) = delete;

    remixapi_ErrorCode initialize(const remixapi_StartupInfo &info);
    remixapi_ErrorCode shutdown();
    remixapi_ErrorCode present(const remixapi_PresentInfo *info);
    remixapi_ErrorCode requestScreenshot(const char *path);
    // A screenshot set in the profiles directory's screenshots folder (runtime/presentation.cpp).
    remixapi_ErrorCode requestScreenshotSet();
    void completeScreenshot();
    remixapi_ErrorCode readPresentedImage(remixapi_PresentedImageEXT &) {
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
    bool modsDisabled() const { return m_disableMods; }
    bool isHeadless() const { return false; }
    bool isInitialized() const { return m_initialized; }
    bool hasFailed() const { return m_failed; }
    bool hasStartedFrame() const { return m_frameStarted; }
    uint64_t presentationCount() const { return m_presentationCount; }
    // With asynchronous presentation the statistics getters return the last
    // completed frame's values while the render thread works (busyState).
    remixapi_FramePerformanceEXT framePerformance() const {
      return busyState() ? busyState()->performance : m_framePerformance;
    }
    remixapi_FrameBreakdownEXT frameBreakdown() const {
      return busyState() ? busyState()->breakdown : m_frameBreakdown;
    }
    // The developer UI's view of the runtime (ui/status.h), the same
    // queries the Vulkan runtime answers (metal/ui.mm).
    ui::DeviceStatus uiDevice() const;
    ui::PreparationStatus uiPreparation() const;
    ui::FrameGenerationStatus uiFrameGeneration() const;
    ui::LatencyStatus uiLatency() const;
    ui::PostStatus uiPost() const;
    ui::PresentStatus uiPresent() const;
    ui::ReconstructionChoices uiReconstructionChoices() const;
    bool reconstructionDenoises() const;
    ui::ReconstructionStatus uiReconstruction() const;
    ui::SkyStatus uiSky() const;
    ui::HdrStatus uiHdr() const;
    ui::ScreenshotStatus uiScreenshots() const;
    void uiTakeScreenshot();
    ui::PathInspectionStatus uiPathInspection() const;
    void uiInspectPixel(int x, int y);
    bool hostPassesSupported() const { return true; } // metal/host_passes.mm
    bool textureCompressionSupported() const;
    // Metal has no opacity micromaps (rtx.native.pathtrace.opacityMicromaps).
    bool opacityMicromapsSupported() const { return false; }
    bool gpuTimersSupported() const;
    metal::Raster::MemoryStatistics rasterMemoryStatistics() const {
      return busyState() ? busyState()->memory : renderer().memoryStatistics();
    }
    size_t pendingDrawCount() const { return m_raster.pendingDrawCount(); }
    // A texture's MTLTexture for the developer UI (ui::addTexture), null when it has none.
    const void *uiTextureView(const scene::Texture *texture) const;
    const auto &textures() const { return m_raster.textures(); }
    const auto &texturePaths() const { return m_raster.paths(); }
    char *uiInput() { return m_input; }
    size_t uiInputSize() const { return sizeof(m_input); }
    // Not ported to Metal: low latency (NVIDIA Reflex) reports inactive.
    remixapi_LocalLightSetStatisticsEXT localLightSetStatistics() const {
      if (const auto *busy = busyState())
        return busy->localLightSet;
      return m_pathTracer && m_backend
                 ? m_pathTracer->localLightSetStatistics()
                 : remixapi_LocalLightSetStatisticsEXT{.structSize = sizeof(remixapi_LocalLightSetStatisticsEXT)};
    }
    remixapi_CameraCullingStatisticsEXT cameraCullingStatistics() const {
      if (const auto *busy = busyState())
        return busy->cameraCulling;
      return m_pathTracer && m_backend
                 ? m_pathTracer->cameraCullingStatistics()
                 : remixapi_CameraCullingStatisticsEXT{.structSize = sizeof(remixapi_CameraCullingStatisticsEXT)};
    }
    bool latencyRequested() const { return false; }
    bool latencyActive() const { return false; }
    // runtime.h AsyncPresentStatistics' fields (Diagnostics reads them by name).
    struct AsyncPresentStatistics {
      bool active = false;
      double waitMilliseconds = 0, setupMilliseconds = 0, captureMilliseconds = 0;
      double uiMilliseconds = 0, callMilliseconds = 0, renderMilliseconds = 0;
      uint64_t handedOff = 0, rendered = 0;
    };
    // The caller's side of the last asynchronous Present (inactive otherwise).
    AsyncPresentStatistics asyncCallerStatistics() const { return m_previousAsyncCaller; }
    // raster/renderer.h Profile's fields; the Metal raster renderer does not
    // profile its prepare() yet.
    struct RasterProfile {
      double prepareMilliseconds = 0, skinningMilliseconds = 0;
      uint64_t culledDraws = 0;
      double retireMilliseconds = 0, refreshMilliseconds = 0, orderMilliseconds = 0, buildMilliseconds = 0;
      uint64_t meshesCreated = 0, materialsCreated = 0, texturesRefreshed = 0;
      double stagingAllocateMilliseconds = 0, stagingWriteMilliseconds = 0, skinUploadMilliseconds = 0;
      uint64_t stagingBuffers = 0, stagingAllocated = 0;
      uint32_t stagingMemoryFlags = 0;
      double recycleMilliseconds = 0, retireMaterialsMilliseconds = 0, retireTexturesMilliseconds = 0;
      double retireMeshesMilliseconds = 0, retirePathsMilliseconds = 0;
      uint64_t materialCount = 0, textureCount = 0, meshCount = 0, pathCount = 0, recycledBuffers = 0;
      uint64_t draws = 0, skinnedVertices = 0, uploadBytes = 0;
      uint64_t haloGroups = 0, haloDraws = 0, maskGroups = 0, maskDraws = 0, digest = 0;
      uint64_t unreadyDraws = 0;
      double createPipelineMilliseconds = 0, createMeshMilliseconds = 0, createTextureMilliseconds = 0;
      double createMaterialMilliseconds = 0;
      uint64_t pipelinesCreated = 0, texturesCreated = 0;
    };
    RasterProfile rasterProfile() const { return {}; }
    RasterProfile takeWorstRasterProfile() { return {}; }
    // raster/renderer.h TextureCosts' fields: Metal textures are created
    // without the Vulkan staging steps it times.
    struct TextureCosts {
      uint64_t images = 0, bytes = 0;
      double query = 0, create = 0, allocate = 0, bind = 0, view = 0, stage = 0, slowest = 0;
    };
    TextureCosts takeTextureCosts() { return {}; }
    // runtime.h WaitedFrame's fields. Presentation is synchronous on Metal
    // (no render thread, no low latency sleep): nothing is waited for.
    struct WaitedFrame {
      double render = 0, present = 0, record = 0, fence = 0, acquire = 0, submit = 0, queuePresent = 0, release = 0;
      double scene = 0, setup = 0, queueLock = 0;
      double raster = 0, surfaces = 0, material = 0, traceAccel = 0, table = 0, passes = 0, composite = 0;
      double geometry = 0, upload = 0, allocation = 0;
      uint64_t bufferAllocations = 0, memoryAllocations = 0;
    };
    WaitedFrame waitedFrame() const { return {}; }
    double asyncCallerWait() const { return 0; }
    double latencySleepMilliseconds() const { return 0; }
    latency::Summary latencySummary() const { return {}; } // not recorded on Metal
    // The compiled and compiling pipelines (pipelines.request).
    std::string pipelineReport() const;
    const gpu::FrameTimings &gpuPassTimings() const { return busyState() ? busyState()->gpuTimings : m_gpuTimings; }
    metal::ReconstructionStatistics reconstructionStatistics() const;
    // The API's scene: every resource call, DrawInstance and retained update.
    metal::Raster &raster() { return m_raster; }
    HostPasses &hostPasses() { return m_hostPasses; }
    RemixSdlWindow &window() { return m_window; }
    remixapi_FeatureAvailability getFeatureAvailability() const {
      remixapi_FeatureAvailability result{};
#ifdef REMIX_HAS_NRD
      // runtime/configuration.cpp: available once the device supports it.
      result.compiledFeatures = REMIXAPI_FEATURE_NRD_BIT;
      if (denoisingCapabilities().reblurShSupported)
        result.availableFeatures = REMIXAPI_FEATURE_NRD_BIT;
#endif
      return result;
    }
    // Apple GPUs support every temporal storage and filtering format.
    remixapi_DenoisingCapabilitiesEXT denoisingCapabilities() const {
      remixapi_DenoisingCapabilitiesEXT result{.structSize = sizeof(remixapi_DenoisingCapabilitiesEXT)};
      result.taaSupported = m_traceSupported;
#ifdef REMIX_HAS_NRD
      result.reblurShSupported = m_traceSupported;
#endif
      return result;
    }
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    remixapi_LocalLightStatisticsEXT localLightStatistics() const;
    remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
    remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
    remixapi_ErrorCode selectBackend(const char *value);
    remixapi_ErrorCode configureTracing(const char *key, const char *value);
    // remixapi_SetConfigDefaultEXT: a host's default (config::Profiles::setDefault).
    remixapi_ErrorCode configureDefault(const char *key, const char *value);
    remixapi_BackendCapabilitiesEXT backendCapabilities() const;
    // remix_sky_ext.h: the game's control over the NUMOS sky (under the API
    // lock), and what the sky did last frame.
    config::SkyControl &skyControl() { return m_skyControl; }
    // What the path tracer's NUMOS sky did last frame (inactive without one).
    config::SkyState skyState() const;
    remixapi_TracingStatisticsEXT tracingStatistics() const;
    remixapi_SharcStatisticsEXT sharcStatistics() const;
    remixapi_ErrorCode configureSetting(const char *key, const char *value);
    remixapi_ErrorCode vramStatistics(remixapi_VramStats &stats) const;
    remixapi_UIState uiState() const { return m_uiState; }
    void setUiState(remixapi_UIState state) {
      m_uiState = state;
      if (state == REMIXAPI_UI_STATE_NONE)
        m_window.setGuiCapture(false, false);
    }
    // After each event pump (remixapi_PumpEvents): the panel's own shortcut.
    void menuShortcut() {
      if (ui::menuShortcut(m_window, m_profiles, m_profileUi, m_uiState != REMIXAPI_UI_STATE_NONE))
        setUiState(m_uiState == REMIXAPI_UI_STATE_NONE ? REMIXAPI_UI_STATE_ADVANCED : REMIXAPI_UI_STATE_NONE);
      // The screenshot shortcut, or the panel's button.
      if (ui::screenshotShortcut(m_window, m_profiles, m_profileUi, m_uiState != REMIXAPI_UI_STATE_NONE) ||
          std::exchange(m_screenshotButton, false))
        requestScreenshotSet();
    }
    scene::FreeCameraState &freeCamera() { return m_freeCamera; }

    struct Gpu; // metal/runtime_gpu.h

  private:
    // Asynchronous presentation ("rtx.native.asyncPresent"; runtime/runtime.h).
    // present() waits for the render thread's previous frame, then runs
    // presentSetup on the caller's thread: settings, the frame capture
    // (publishFrame into m_frameScene), particles and the developer UI build,
    // published statistics; renderFrame records, commits and presents it on
    // the render thread. Synchronous presentation runs both on the caller.
    //
    // Ownership while the render thread works (from the handoff to the next
    // waitRenderThread): API callers keep m_raster (the API's scene), the
    // profiles, m_skyControl, the window and m_published; the render thread
    // owns m_frameScene (with the GPU state), the tracer, the particles, the
    // frame slots, the screenshot readback, the ImGui draw data and the
    // per-frame statistics. It reads the frame's settings from m_frameSettings
    // and m_frameSkyControl, copied at the handoff.
    remixapi_ErrorCode presentSetup(bool &render);
    void renderFrame();
    void renderFrameUnguarded();
    double waitRenderThread();
    void switchPresentation(bool async);
    struct Published {
      remixapi_FramePerformanceEXT performance{};
      remixapi_FrameBreakdownEXT breakdown{};
      remixapi_TracingStatisticsEXT tracing{};
      remixapi_SharcStatisticsEXT sharc{};
      remixapi_ShadowStatisticsEXT shadow{};
      remixapi_LocalLightStatisticsEXT localLight{};
      remixapi_LocalLightSetStatisticsEXT localLightSet{};
      remixapi_CameraCullingStatisticsEXT cameraCulling{};
      remixapi_LocalLightGuidingStatisticsEXT guiding{};
      remixapi_DenoisingStatisticsEXT denoising{};
      metal::ReconstructionStatistics reconstruction;
      metal::Raster::MemoryStatistics memory;
      remixapi_VramStats vram{};
      remixapi_ErrorCode vramStatus = REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
      gpu::FrameTimings gpuTimings;
    };
    void publishState();
    // Non-null while the render thread works on a frame.
    const Published *busyState() const { return m_asyncActive && !m_renderThread.idle() ? &m_published : nullptr; }
    // The renderer that owns the GPU state and the frame being rendered: the
    // API's own (synchronous presentation) or the published frame's.
    metal::Raster &renderer() { return m_asyncActive ? *m_frameScene : m_raster; }
    const metal::Raster &renderer() const { return m_asyncActive ? *m_frameScene : m_raster; }
    // The frame's settings: the profile's (synchronous), or the copy the
    // render thread reads (asynchronous).
    const config::Settings &frameSettings() const { return m_asyncActive ? m_frameSettings : m_profiles.current(); }
    const config::SkyControl &frameSkyControl() const { return m_asyncActive ? m_frameSkyControl : m_skyControl; }
    std::unique_ptr<metal::Raster> m_frameScene;
    RenderThread m_renderThread;
    bool m_asyncActive = false;
    Published m_published;
    config::Settings m_frameSettings;
    config::SkyControl m_frameSkyControl;
    AsyncPresentStatistics m_asyncCaller, m_previousAsyncCaller; // caller-owned
    double m_handoffMilliseconds = 0; // the caller's setup and capture, for the frame's Present time
    config::SkyControl m_skyControl;
    remixapi_ErrorCode fail(const char *reason);
    void completeScreenshot(bool wait);
    void applySettings();
    // Activates the requested backend; a path tracer whose configuration
    // differs is prepared on a thread while the current one keeps rendering.
    void prepareSettings(uint32_t width, uint32_t height);
    bool preparationPending() const;
    double preparationSeconds() const;
    void updateFreeCamera();
    bool initializeUi();
    void shutdownUi();
    void buildUi();
    void renderUi(void *encoder); // an id<MTLRenderCommandEncoder>
    // The raster backend's NUMOS sky (metal/raster_sky.mm): its frame, before
    // the target's pass, and after the world's layers the screen clouds and the
    // composite images (outside the pass; true: Raster::Gpu::recordSkyComposite
    // blends them). Both take an id<MTLCommandBuffer>.
    bool recordRasterSky(void *command);
    bool composeRasterSky(void *command);
    // Particle emitters (metal/runtime_particles.mm, runtime/particles.cpp):
    // publishParticles at Present takes the registry's frame; simulateParticles
    // records its passes outside the target pass (an id<MTLCommandBuffer>),
    // drawParticles its draws inside it (an id<MTLRenderCommandEncoder>).
    // HDR10 output (metal/hdr_output.mm, runtime/hdr_output.h): configureHdr
    // before the frame takes its formats; scaleHdrUi brackets the HUD and the
    // developer UI (an id<MTLRenderCommandEncoder>); encodeHdr writes the back
    // buffer into the drawable's texture (an id<MTLCommandBuffer>, id<MTLTexture>).
    void configureHdr();
    scene::DisplayOutput displayOutput() const;
    void scaleHdrUi(void *encoder, bool before);
    void encodeHdr(void *command, void *drawableTexture);
    void publishParticles();
    bool simulateParticles(void *command);
    void drawParticles(void *encoder);
    scene::ParticleFrame m_particleFrame;
    std::chrono::steady_clock::time_point m_particleClock{}, m_particleStart = std::chrono::steady_clock::now();
    uint32_t m_particleUpAxis = 2; // the up axis the last traced frame resolved
    // The panel's settings tabs, shared with Vulkan builds (ui/settings.cpp,
    // dispatched by ui::Tab; the tool tabs have their own ui/ sources).
    void drawSettingsTab(int tab, config::Settings &settings);
    void drawDisplayTab(config::Settings &settings);
    void drawLightingTab(config::Settings &settings);
    void drawMaterialsTab(config::Settings &settings);
    void drawSkyTab(config::Settings &settings);
    void drawPostProcessTab(config::Settings &settings);
    void drawPerformanceTab(config::Settings &settings);
    void drawDiagnosticsTab(config::Settings &settings);
    void drawCaptureTab(config::Settings &settings);
    void drawGpuPassTimings(bool enabled);
    void drawTexturesTab(config::Settings &settings);
    // The diagnostics tab's path inspector (uiPathInspection).
    bool m_inspecting = false;
    int m_inspectX = -1, m_inspectY = -1;
    void drawPathInspector();
    static bool processEvent(const SDL_Event *event, void *userData);
    void destroyMetal();

    RemixSdlWindow m_window;
    metal::Raster m_raster;
    HostPasses m_hostPasses;
    std::unique_ptr<Gpu> m_gpu;
    std::unique_ptr<metal::PathTracer> m_pathTracer;
    std::shared_ptr<metal::PreparedTracer> m_preparation;
    std::vector<std::shared_ptr<metal::PreparedTracer>> m_obsoletePreparations;
    std::optional<metal::PathTracer::Configuration> m_failedConfiguration;
    bool m_localSourcesSeen = false, m_loading = false;
    uint64_t m_preparationGeneration = 0; // configurations prepared
    scene::FreeCameraState m_freeCamera;
    config::Profiles m_profiles;
    config::Settings m_appliedSettings;
    ui::ProfileUi m_profileUi;
    TraceOptions m_traceOptions;
    gpu::FrameTimings m_gpuTimings;
    remixapi_FramePerformanceEXT m_framePerformance{.structSize = sizeof(remixapi_FramePerformanceEXT)};
    remixapi_FrameBreakdownEXT m_frameBreakdown{.structSize = sizeof(remixapi_FrameBreakdownEXT)};
    // The previous Present's costs, for Diagnostics (drawn while the current one records).
    remixapi_FramePerformanceEXT m_previousPerformance{.structSize = sizeof(remixapi_FramePerformanceEXT)};
    remixapi_FrameBreakdownEXT m_previousBreakdown{.structSize = sizeof(remixapi_FrameBreakdownEXT)};
    char m_input[256] = {}; // Diagnostics' text input test field
    std::string m_traceUnavailable, m_failure;
    PresentedScreenshot m_screenshot;
    bool m_screenshotButton = false; // the post-processing tab's button, for menuShortcut
    std::chrono::steady_clock::time_point m_lastPresentation{};
    double m_longestPresentationGap = 0;
    uint64_t m_presentationCount = 0, m_sharcReset = 0;
    uint32_t m_backend = 0;
    float m_clearColor[4] = {0.025f, 0.035f, 0.055f, 1.0f};
    remixapi_UIState m_uiState = REMIXAPI_UI_STATE_NONE;
    ImGuiContext *m_gui = nullptr;
    bool m_initialized = false, m_frameStarted = false, m_vsync = false;
    bool m_traceSupported = false, m_disableProfiles = false, m_disableMods = false;
    std::atomic<bool> m_failed{false};
  };
} // namespace remix
