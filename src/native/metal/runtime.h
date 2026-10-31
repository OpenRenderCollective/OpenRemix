#pragma once
// The Metal runtime: the API's runtime in macOS builds
// (runtime/runtime.h in Vulkan builds). The public interface is the one the
// API layer uses; Metal state is private to metal/*.mm.
#include "metal/diagnostics.h"
#include "metal/host_passes.h"
#include "metal/raster.h"
#include "metal/tracer.h"
#include "pathtrace/options.h"
#include "metal/ui/status.h"
#include "platform/gpu_timings.h"
#include "platform/window.h"
#include "runtime/profiles.h"
#include "runtime/sky_control.h"
#include "scene/free_camera.h"
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
    void completeScreenshot();
    bool modsDisabled() const { return m_disableMods; }
    bool isHeadless() const { return false; }
    bool isInitialized() const { return m_initialized; }
    bool hasFailed() const { return m_failed; }
    bool hasStartedFrame() const { return m_frameStarted; }
    uint64_t presentationCount() const { return m_presentationCount; }
    remixapi_FramePerformanceEXT framePerformance() const { return m_framePerformance; }
    remixapi_FrameBreakdownEXT frameBreakdown() const { return m_frameBreakdown; }
    // The developer UI's view of the runtime (metal/ui/status.h), the same
    // queries the Vulkan runtime answers (metal/ui.mm).
    ui::DeviceStatus uiDevice() const;
    ui::PreparationStatus uiPreparation() const;
    ui::FrameGenerationStatus uiFrameGeneration() const;
    ui::PostStatus uiPost() const;
    ui::PresentStatus uiPresent() const;
    ui::ReconstructionChoices uiReconstructionChoices() const;
    bool textureCompressionSupported() const;
    bool gpuTimersSupported() const;
    auto rasterMemoryStatistics() const { return m_raster.memoryStatistics(); }
    size_t pendingDrawCount() const { return m_raster.pendingDrawCount(); }
    // A texture's MTLTexture for the developer UI (ui::addTexture), null when it has none.
    const void *uiTextureView(const scene::Texture *texture) const;
    const auto &textures() const { return m_raster.textures(); }
    const auto &texturePaths() const { return m_raster.paths(); }
    char *uiInput() { return m_input; }
    size_t uiInputSize() const { return sizeof(m_input); }
    // Not ported to Metal: local light sets, camera culling, low latency
    // (NVIDIA Reflex) and asynchronous presentation report inactive.
    remixapi_LocalLightSetStatisticsEXT localLightSetStatistics() const { return {.structSize = sizeof(remixapi_LocalLightSetStatisticsEXT)}; }
    remixapi_CameraCullingStatisticsEXT cameraCullingStatistics() const { return {.structSize = sizeof(remixapi_CameraCullingStatisticsEXT)}; }
    bool latencyRequested() const { return false; }
    bool latencyActive() const { return false; }
    // runtime.h AsyncPresentStatistics' fields (Diagnostics reads them by name).
    struct AsyncPresentStatistics {
      bool active = false;
      double waitMilliseconds = 0, setupMilliseconds = 0, captureMilliseconds = 0;
      double uiMilliseconds = 0, callMilliseconds = 0, renderMilliseconds = 0;
      uint64_t handedOff = 0, rendered = 0;
    };
    AsyncPresentStatistics asyncCallerStatistics() const { return {}; }
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
    // The compiled and compiling pipelines (pipelines.request).
    std::string pipelineReport() const;
    const gpu::FrameTimings &gpuPassTimings() const { return m_gpuTimings; }
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
    // remix_sky_ext.h: the Metal backend has no NUMOS sky; the control is
    // kept and the state stays inactive.
    config::SkyControl &skyControl() { return m_skyControl; }
    config::SkyState skyState() const { return {}; }
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
    }
    scene::FreeCameraState &freeCamera() { return m_freeCamera; }

    struct Gpu; // metal/runtime_gpu.h

  private:
    config::SkyControl m_skyControl;
    remixapi_ErrorCode fail(const char *reason);
    remixapi_ErrorCode presentFrame();
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
    // The developer UI's tabs (ui/rendering.cpp, lighting.cpp,
    // post_processing.cpp, diagnostics.cpp), shared with Vulkan builds.
    void drawRenderingTab(config::Settings &settings);
    void drawLightingTab(config::Settings &settings);
    void drawSkyTab(config::Settings &settings);
    void drawPostProcessingTab(config::Settings &settings);
    void drawDiagnosticsTab(config::Settings &settings);
    void drawGpuPassTimings(bool enabled);
    void drawTexturesTab(config::Settings &settings);
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
    std::string m_screenshotPath;
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
