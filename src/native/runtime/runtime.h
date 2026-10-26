/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "pathtrace/renderer.h"
#include "particles/renderer.h"
#include "platform/gpu_frames.h"
#include "platform/gpu_timers.h"
#include "platform/present_queue.h"
#include "platform/render_thread.h"
#include "platform/window.h"
#include "raster/renderer.h"
#include "runtime/frame_generation.h"
#include "runtime/hdr_output.h"
#include "runtime/host_passes.h"
#include "runtime/pipelines.h"
#include "runtime/profiles.h"
#include "runtime/screenshot.h"
#include "ui/menu_shortcut.h"
#include "ui/profiles.h"
#include "scene/free_camera.h"
#include "scene/up_axis.h"
#include <remix/remix_c.h>
#include <remix/remix_editor_ext.h>
#include <atomic>
#include <vector>
#include <vulkan/vulkan.h>

struct ImGuiContext;

namespace remix {
class FrameSnapshot;
namespace sky {
  class Renderer;
}
class RemixNativeRuntime {
public:
  RemixNativeRuntime() = default;
  ~RemixNativeRuntime();
  RemixNativeRuntime(const RemixNativeRuntime &) = delete;
  RemixNativeRuntime &operator=(const RemixNativeRuntime &) = delete;

  remixapi_ErrorCode initialize(const remixapi_StartupInfo &info);
  remixapi_ErrorCode shutdown();
  // Statistics getters. With asynchronous presentation they return the last
  // completed frame's values: live when the render thread is idle, otherwise
  // those published when Present handed the current frame over.
  upscale::Statistics reconstructionStatistics() const;
  postfx::Statistics postProcessingStatistics() const {
    if (const auto *busy = busyState())
      return busy->post;
    return m_postProcessing ? m_postProcessing->statistics() : postfx::Statistics{};
  }
  const std::string &postProcessingFailure() const { return m_postFailure; }
  remixapi_ErrorCode present(const remixapi_PresentInfo *info);
  bool lowLatencySupported() const { return m_lowLatencySupported; }
  remixapi_ErrorCode requestScreenshot(const char *path);
  void completeScreenshot();
  int configureSharedFrame(remixapi_EditorSharedFrame &frame);
  bool modsDisabled() const { return m_disableMods; }
  bool isHeadless() const { return m_headless; }
  bool isInitialized() const { return m_initialized; }
  uint64_t presentationCount() const {
    if (const auto *busy = busyState())
      return busy->presentations;
    return m_presentationCount;
  }
  remixapi_FramePerformanceEXT framePerformance() const {
    if (const auto *busy = busyState())
      return busy->performance;
    return m_framePerformance;
  }
  remixapi_FrameBreakdownEXT frameBreakdown() const {
    if (const auto *busy = busyState())
      return busy->breakdown;
    return m_frameBreakdown;
  }
  // GPU pass timers ("rtx.native.gpuPassTimers"): the newest completed frame.
  const gpu::FrameTimings &gpuPassTimings() const {
    if (const auto *busy = busyState())
      return busy->gpuTimings;
    return m_gpuTimers.latest();
  }
  // Diagnostic digest of the last traced frame's GPU inputs (REMIX_FRAME_DIGEST).
  uint64_t frameDigest() const {
    if (const auto *busy = busyState())
      return busy->digest;
    return (m_pathTracer ? m_pathTracer->frameDigest() : 0) * 31 + renderer().profile().digest;
  }
  // Asynchronous presentation ("rtx.native.asyncPresent"), for Diagnostics.
  struct AsyncPresentStatistics {
    bool active = false;
    // Caller thread, one Present: waiting for the render thread to finish
    // the previous frame (the handoff), settings/swapchain/pipelines/UI
    // setup, frame capture (publishFrame), the developer UI build (part of
    // setup), and the whole call (all of the above).
    double waitMilliseconds = 0, setupMilliseconds = 0, captureMilliseconds = 0;
    double uiMilliseconds = 0, callMilliseconds = 0;
    // Render thread, one frame: scene preparation through queue present.
    double renderMilliseconds = 0;
    uint64_t handedOff = 0; // frames handed to the render thread
    uint64_t rendered = 0;  // of those, frames the render thread presented
  };
  struct PreparationStatus {
    bool pending, loading;
    pipeline::Statistics pipelines;
    std::string failure;
  };
  PreparationStatus preparationStatus() const {
    return {preparationPending(), m_loading,
            m_pipelines ? m_pipelines->statistics() : pipeline::Statistics{},
            m_preparationFailure};
  }
  std::string pipelineReport() const {
    return m_pipelines ? m_pipelines->report() : std::string("no device\n");
  }
  bool hasStartedFrame() const {
    if (const auto *busy = busyState())
      return busy->frameStarted;
    return m_frameStarted;
  }
  bool hasFailed() const { return m_failed; }
  // The API's scene: every resource call, DrawInstance and retained update.
  RasterRenderer &raster() { return m_raster; }
  HostPasses &hostPasses() { return m_hostPasses; }
  RemixSdlWindow &window() { return m_window; }
  remixapi_FeatureAvailability getFeatureAvailability() const;
  remixapi_DenoisingCapabilitiesEXT denoisingCapabilities() const {
    return m_denoiseCapabilities;
  }
  remixapi_ShadowStatisticsEXT shadowStatistics() const;
  remixapi_LocalLightStatisticsEXT localLightStatistics() const;
  remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
  remixapi_LocalLightSetStatisticsEXT localLightSetStatistics() const;
  remixapi_CameraCullingStatisticsEXT cameraCullingStatistics() const;
  remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
  remixapi_ErrorCode selectBackend(const char *value);
  remixapi_ErrorCode configureTracing(const char *key, const char *value);
  // remixapi_SetConfigDefaultEXT: a host's default (config::Profiles::setDefault).
  remixapi_ErrorCode configureDefault(const char *key, const char *value);
  remixapi_BackendCapabilitiesEXT backendCapabilities() const;
  // remix_sky_ext.h: the game's control over the sky (under the API lock),
  // and what the sky did last frame.
  config::SkyControl &skyControl() { return m_skyControl; }
  config::SkyState skyState() const;
  remixapi_TracingStatisticsEXT tracingStatistics() const;
  remixapi_SharcStatisticsEXT sharcStatistics() const;
  // Low latency: asked for by the setting, and in effect (the path tracing
  // backend or frame generation).
  bool latencyRequested() const { return m_lowLatencyRequested; }
  bool latencyActive() const { return m_lowLatencyActive; }
  // The raster renderer's last prepare() (read while the render thread may
  // be writing it: diagnostics only).
  RasterRenderer::Profile rasterProfile() const { return renderer().completedProfile(); }
  RasterRenderer::Profile takeWorstRasterProfile() { return renderer().takeWorstProfile(); }
  RasterRenderer::TextureCosts takeTextureCosts() { return renderer().takeTextureCosts(); }
  // How long the Present just made waited for the render thread, and the
  // frame it waited for: the render thread's whole time on it and its parts.
  double asyncCallerWait() const { return m_asyncCaller.waitMilliseconds; }
  struct WaitedFrame {
    double render = 0, present = 0, record = 0, fence = 0, acquire = 0, submit = 0, queuePresent = 0, release = 0;
    double scene = 0, setup = 0, queueLock = 0;
    // Of record: the raster renderer's preparation, the tracer's surfaces
    // (materials within), acceleration structures, tables and passes, and
    // within those its geometry, uploads and allocations.
    double raster = 0, surfaces = 0, material = 0, traceAccel = 0, table = 0, passes = 0, composite = 0;
    double geometry = 0, upload = 0, allocation = 0;
    uint64_t bufferAllocations = 0, memoryAllocations = 0;
  };
  WaitedFrame waitedFrame() const { return m_waitedFrame; }
  // How long the last Present's low latency sleep held the host.
  double latencySleepMilliseconds() const { return m_latencySleepMilliseconds; }
  // The caller's side of the last asynchronous Present (inactive otherwise).
  AsyncPresentStatistics asyncCallerStatistics() const { return m_previousAsyncCaller; }
  remixapi_ErrorCode configureSetting(const char *key, const char *value);
  // GetVramStats, gated by the "Report VRAM statistics" setting.
  remixapi_ErrorCode vramStatistics(remixapi_VramStats &stats) const;
  remixapi_UIState uiState() const { return m_uiState; }
  void setUiState(remixapi_UIState state) {
    m_uiState = state;
    if (state == REMIXAPI_UI_STATE_NONE) {
      m_window.setGuiCapture(false, false);
    }
  }
  // After each event pump (remixapi_PumpEvents): the panel's own shortcut.
  void menuShortcut() {
    if (ui::menuShortcut(m_window, m_profiles, m_profileUi, m_uiState != REMIXAPI_UI_STATE_NONE))
      setUiState(m_uiState == REMIXAPI_UI_STATE_NONE ? REMIXAPI_UI_STATE_ADVANCED : REMIXAPI_UI_STATE_NONE);
  }
  scene::FreeCameraState &freeCamera() { return m_freeCamera; }
  const scene::FreeCameraState &freeCamera() const { return m_freeCamera; }

private:
  struct QueueSelection {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t family = VK_QUEUE_FAMILY_IGNORED;
  };
  remixapi_ErrorCode createInstance();
  remixapi_ErrorCode selectPhysicalDevice(QueueSelection &selection) const;
  remixapi_ErrorCode createDevice(const QueueSelection &selection);
  remixapi_ErrorCode createSwapchain();
  remixapi_ErrorCode recreateSwapchain();
  remixapi_ErrorCode createFrameResources();
  remixapi_ErrorCode createAttachments();
  remixapi_ErrorCode recordFrame(uint32_t imageIndex, scene::TraceFrame *traceScene,
                                 const GeneratedImages &generated = {});
  // Synchronous presentation: the whole frame on the calling thread.
  remixapi_ErrorCode presentFrame(const remixapi_PresentInfo *info);
  // Asynchronous presentation. presentAsync runs on the caller's thread with
  // the render thread idle: setup, frame capture (publishFrame into
  // m_frameScene), developer UI build, published statistics, handoff.
  // renderFrame runs on the render thread without the API mutex and touches
  // only render-owned state (see the members below).
  remixapi_ErrorCode presentAsync(const remixapi_PresentInfo *info,
                                  std::chrono::steady_clock::time_point start, double waited);
  void renderFrame();
  // Waits until the render thread has finished its frame (zero when it never
  // ran). Returns the milliseconds waited.
  double waitRenderThread();
  // Render thread idle: moves the GPU state between the API's renderer and
  // the published frame's, and restarts the static set's epoch.
  void switchPresentation(bool async);
  // What the statistics getters return while the render thread works: the
  // last completed frame's values, taken at the handoff. Written and read
  // under the API mutex only.
  struct Published {
    remixapi_FramePerformanceEXT performance{};
    remixapi_FrameBreakdownEXT breakdown{};
    uint64_t presentations = 0, digest = 0;
    bool frameStarted = false;
    upscale::Statistics upscaling;
    postfx::Statistics post;
    remixapi_TracingStatisticsEXT tracing{};
    remixapi_SharcStatisticsEXT sharc{};
    remixapi_ShadowStatisticsEXT shadow{};
    remixapi_LocalLightStatisticsEXT localLight{};
    remixapi_LocalLightSetStatisticsEXT localLightSet{};
    remixapi_CameraCullingStatisticsEXT cameraCulling{};
    remixapi_LocalLightGuidingStatisticsEXT guiding{};
    remixapi_DenoisingStatisticsEXT denoising{};
    RasterRenderer::MemoryStatistics memory;
    gpu::FrameTimings gpuTimings;
  };
  void publishState();
  // Non-null while the render thread works on a frame. Only Present hands a
  // frame over, under the API mutex, so a caller holding the mutex that sees
  // null keeps the render thread idle until it releases the mutex.
  const Published *busyState() const {
    return m_asyncActive && !m_renderThread.idle() ? &m_published : nullptr;
  }
  // The renderer that owns the GPU state and the frame being rendered: the
  // API's own (synchronous presentation) or the published frame's.
  RasterRenderer &renderer() { return m_asyncActive ? *m_frameScene : m_raster; }
  const RasterRenderer &renderer() const { return m_asyncActive ? *m_frameScene : m_raster; }
  remixapi_ErrorCode fail(VkResult result = VK_ERROR_UNKNOWN);
  void destroySharedFrame();
  void sharedBarrier(VkCommandBuffer command, bool acquire);
  void destroySwapchain();
  void destroyVulkan();
  bool supportsDeviceExtension(VkPhysicalDevice device, const char *name) const;
  // Sets m_hdr10Requested and m_hdrStatus.
  VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats);
  // HDR10 output is wanted: the setting is on and Windows has HDR on for the
  // window's display (not headless; VK_EXT_swapchain_colorspace enabled).
  bool wantsHdr10() const;
  // The tone curve's peak and scale for displayColor (1 and 1 without HDR10).
  PathTracer::DisplayOutput displayOutput() const;
  VkPresentModeKHR
  choosePresentMode(const std::vector<VkPresentModeKHR> &modes) const;
  bool initializeUi();
  void shutdownUi();
  bool initializeUiVulkan();
  void shutdownUiVulkan();
  void drawUi(VkCommandBuffer commandBuffer);
  void drawRenderingTab(config::Settings &settings);
  void drawLightingTab(config::Settings &settings);
  void drawSkyTab(config::Settings &settings);
  void drawPostProcessingTab(config::Settings &settings);
  void drawDiagnosticsTab(config::Settings &settings);
  void drawTexturesTab(config::Settings &settings);
  VkImageView textureView(const scene::Texture *texture) const {
    const VkImageView view = m_raster.textureView(texture);
    return view != VK_NULL_HANDLE ? view : (m_asyncActive && m_frameScene ? m_frameScene->textureView(texture) : VK_NULL_HANDLE);
  }
  const std::unordered_map<uint64_t, std::shared_ptr<scene::Texture>> &textures() const { return m_raster.textures(); }
  const std::unordered_map<std::filesystem::path, std::weak_ptr<scene::Texture>> &texturePaths() const { return m_raster.paths(); }
  void drawGpuPassTimings(bool enabled);
  void applySettings();
  void prepareSettings();
  void preparePostProcessing();
  void presented();
  void retireTracer(std::unique_ptr<PathTracer> tracer,
                    std::shared_ptr<void> depth);
  bool preparationPending() const;
  // Frame generation ("rtx.native.frameGeneration", runtime/frame_generation.h).
  // prepareFrameGeneration: Present setup (render thread idle): creates the
  // feature for the current swapchain and render size and reports whether
  // this frame can generate. DLSS's is created there; FSR's compiles
  // pipelines and is prepared on the pipeline service's worker
  // (m_frameGenerationPreparation). acquireGenerated: the frame's second swapchain
  // image. recordFrameGeneration: the end of the frame's command buffer.
  // presentImages: hands the frame's images to the presenter thread, which
  // shows them as the GPU finishes the frame: the generated ones first, the
  // rendered one last, at even steps of the frame. Those presents overlap the
  // next frames' work on the other threads, so every use of the queue and of
  // the swapchain is made under m_queueMutex (acquireImage polls under it
  // instead of blocking in the driver). finishDeferredPresent waits for the
  // presenter (before the swapchain or the device goes); takeDeferredResult
  // acts on what its presents returned.
  void takeDeferredResult();
  VkResult acquireImage(VkSemaphore semaphore, uint64_t timeoutNanoseconds, uint32_t &imageIndex);
  // NVIDIA Reflex (VK_NV_low_latency2, "rtx.native.lowLatency"). The driver
  // is told where each frame's phases begin and end (latencyMarker) and, once
  // per frame after Present, holds the host back until rendering it will not
  // queue it behind the GPU (latencySleep): the host's next frame then starts
  // from fresher input. applyLatencyMode: render thread idle.
  remixapi_ErrorCode presentUnpaced(const remixapi_PresentInfo *info);
  void applyLatencyMode();
  void latencySleep();
  // Whether the driver's low latency mode should be on now.
  bool latencyWanted() const;
  void latencyMarker(VkLatencyMarkerNV marker, uint64_t frame);
  // "openremix_framegen.log" beside the runtime library, written once frame
  // generation has been asked for: the device, each swapchain, every change
  // of status and, every few seconds, the counters. What a user sends when
  // generated frames do not appear. logFrameGenerationState: once a Present,
  // on the caller's thread.
  void logFrameGeneration(const char *format, ...);
  void logFrameGenerationState();
  void prepareFrameGeneration();
  GeneratedImages acquireGenerated();
  void recordFrameGeneration(VkCommandBuffer command, uint32_t realIndex, const GeneratedImages &generated);
  VkResult presentImages(uint32_t realIndex, const GeneratedImages &generated);
  void finishDeferredPresent();
  static bool processEvent(const SDL_Event *event, void *userData);
  void updateFreeCamera();
  // Particle emitters (runtime/particles.cpp). publishParticles: Present
  // setup (render thread idle): the registry's frame for the render thread
  // and the last completed frame's statistics for the registry.
  // simulateParticles: the frame's compute passes, outside a render pass;
  // true when drawParticles has something to draw inside the target pass,
  // after the world layer (it captures the scene's depth first if no one has).
  void publishParticles();
  bool simulateParticles(VkCommandBuffer buffer);
  void drawParticles(VkCommandBuffer buffer, VkRenderPassBeginInfo &pass);

  RemixSdlWindow m_window;
  RasterRenderer m_raster;
  // Asynchronous presentation. Ownership while the render thread works on a
  // frame (between Present's handoff and the next waitRenderThread):
  // - API mutex holders (every API call): m_raster (the API's scene), the
  //   profiles and settings, m_uiState, the window, m_published.
  // - The render thread: m_frameScene (the published frame, the static set
  //   replica and the GPU state), the tracer, post-processing, frame slots,
  //   command buffers, the swapchain images, m_queue, the screenshot readback,
  //   the last-frame snapshot, the ImGui draw data and the per-frame
  //   statistics (m_framePerformance, m_frameBreakdown, m_staticStatistics,
  //   m_presentationCount, m_frameStarted, m_asyncRender).
  // - Written by Present's setup only (render thread idle) and read by the
  //   render thread: settings derived for the frame (m_traceOptions,
  //   m_loading, m_backend, m_clearColor, ...), the swapchain and passes.
  // m_failed is atomic: the render thread may fail a frame while API calls
  // check it.
  std::unique_ptr<RasterRenderer> m_frameScene;
  RenderThread m_renderThread;
  bool m_asyncActive = false;
  Published m_published;
  AsyncPresentStatistics m_asyncCaller, m_previousAsyncCaller; // caller-owned
  AsyncPresentStatistics m_asyncRender;                         // render-owned
  struct Handoff {
    double callerMilliseconds = 0; // setup and capture on the caller's thread
    bool memoryStatistics = false; // take m_renderedMemory ("Report VRAM statistics")
  } m_handoff;                     // caller writes before the handoff; render thread reads
  RasterRenderer::MemoryStatistics m_renderedMemory; // render-owned: after its last frame
  scene::FreeCameraState m_freeCamera;
  std::shared_ptr<upscale::Ngx> m_ngx;
  std::unique_ptr<PathTracer> m_pathTracer;
  // The raster backend's NUMOS sky (raster_sky.cpp): recordRasterSky before
  // the target pass, false without a sky this frame; compositeRasterSky after
  // the world's layers, inside the pass, which it leaves and re-enters.
  bool recordRasterSky(VkCommandBuffer buffer, RasterRenderer &raster);
  void compositeRasterSky(VkCommandBuffer buffer, RasterRenderer &raster, VkRenderPassBeginInfo &pass);
  std::shared_ptr<sky::Renderer> m_rasterSky;
  std::string m_rasterSkyFailure;
  scene::Matrix m_rasterSkyView{}, m_rasterSkyProjection{};
  bool m_rasterSkyReady = false, m_rasterSkyActive = false;
  scene::UpAxisDetector m_rasterUpAxis;
  std::shared_ptr<postfx::State> m_exposureState;
  std::unique_ptr<postfx::Renderer> m_postProcessing;
  struct PreparedPost {
    pipeline::PreparationQueue::Ticket job;
    std::unique_ptr<postfx::Renderer> renderer;
    std::string key;
  };
  std::shared_ptr<PreparedPost> m_postPreparation;
  std::vector<std::shared_ptr<PreparedPost>> m_obsoletePost;
  std::string m_postFailure, m_failedPostKey;
  TraceOptions m_postOptions;
  uint64_t m_postGeneration = 0;
  std::unique_ptr<pipeline::Service> m_pipelines;
  struct PreparedConfiguration {
    pipeline::PreparationQueue::Ticket job;
    std::unique_ptr<PathTracer> tracer;
    std::shared_ptr<void> depth;
    std::string key;
    uint64_t generation{}, revision{};
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
  };
  std::shared_ptr<PreparedConfiguration> m_preparation;
  // Obsolete jobs may still be inside an uncancellable driver call. They
  // cannot activate, and must keep their dependencies until completion.
  std::vector<std::shared_ptr<PreparedConfiguration>> m_obsoletePreparations;
  std::string m_activePreparationKey, m_failedPreparationKey,
      m_preparationFailure;
  pipeline::LatestConfiguration m_configurationRevision;
  uint64_t m_preparationGeneration = 0;
  bool m_loading = false, m_rasterPreparing = false;
  uint32_t m_reconstructionFailureMask = 0;
  std::string m_reconstructionFailureKey, m_reconstructionFailure;
  bool m_localSourcesSeen = false;
  // When an upscaler profile first met a non-perspective camera (a menu or
  // loading screen) with no renderer yet: prepareSettings holds the first
  // preparation for the world's camera, within a bound.
  std::chrono::steady_clock::time_point m_worldCameraWait{};
  bool m_worldCameraHold = false; // holding now: not loading, nothing pending
  std::shared_ptr<std::atomic<unsigned>> m_ngxPreparing =
      std::make_shared<std::atomic<unsigned>>(0);
  TraceOptions m_traceOptions;
  config::Profiles m_profiles;
  config::Settings m_appliedSettings;
  ui::ProfileUi m_profileUi;
  uint64_t m_sharcReset = 0;
  uint32_t m_targetBackend = 0;
  bool m_backendFailed = false;
  uint32_t m_backend = 0, m_requestedBackend = 0,
           m_instanceApiVersion = VK_API_VERSION_1_1;
  bool m_traceSupported = false;
  bool m_textureCompression = false; // textureCompressionBC enabled on m_device.
  bool m_memoryBudget = false;       // VK_EXT_memory_budget queries available.
  remixapi_DenoisingCapabilitiesEXT m_denoiseCapabilities{
      .structSize = sizeof(remixapi_DenoisingCapabilitiesEXT)};
  std::string m_traceUnavailable;
  // Why the NUMOS sky cannot run on the device, or empty.
  std::string m_skyUnavailable;
  config::SkyControl m_skyControl;
  PresentedScreenshot m_screenshot;
  bool m_screenshotSupported = false;
  std::shared_ptr<FrameSnapshot> m_lastFrame;
  HostPasses m_hostPasses; // remixapi_HostPassEXT replay; needs the snapshot's swapchain usage
  // Particles: render-owned, except the frame Present publishes (render thread idle).
  std::unique_ptr<particles::Renderer> m_particles;
  scene::ParticleFrame m_particleFrame;
  std::chrono::steady_clock::time_point m_particleClock{}, m_particleStart = std::chrono::steady_clock::now();
  uint32_t m_particleUpAxis = 2; // the up axis the last traced frame resolved
  bool m_snapshotSupported = false, m_snapshotAllocationFailed = false;
  bool m_initialized = false;
  uint64_t m_presentationCount = 0;
  std::chrono::steady_clock::time_point m_lastPresentation{};
  double m_longestPresentationGap = 0;
  remixapi_FramePerformanceEXT m_framePerformance{
      .structSize = sizeof(remixapi_FramePerformanceEXT)};
  remixapi_FrameBreakdownEXT m_frameBreakdown{
      .structSize = sizeof(remixapi_FrameBreakdownEXT)};
  // Completed values of the previous attempt, for the Diagnostics panel.
  remixapi_FramePerformanceEXT m_previousPerformance{};
  remixapi_FrameBreakdownEXT m_previousBreakdown{};
  // The persistent static set of this and the previous attempt.
  PathTracer::StaticStatistics m_staticStatistics{}, m_previousStatic{};
  bool m_frameStarted = false;
  std::atomic<bool> m_failed{false};
  bool m_deviceLost = false;
  bool m_rebuild = false;
  bool m_recreateSurface = false;
  bool m_vsync = false;
  bool m_presenterVsync = false;
  // Frame generation, in generated frames per rendered frame (0: off).
  // Requested: the setting; presenter: what the swapchain was created for
  // (that many more images); m_generateCount: what this frame generates.
  uint32_t m_frameGenerationRequested = 0, m_presenterFrameGeneration = 0, m_generateCount = 0;
  bool m_frameGenerationReady = false, m_frameGenerationFailed = false;
  // "rtx.native.frameGenerationBackend", and the value prepareFrameGeneration
  // last acted on: a change replaces the object, and retries after a failure
  // of the other backend, through a new swapchain.
  upscale::FrameGenerationMode m_frameGenerationBackend = upscale::FrameGenerationMode::Auto,
                               m_appliedFrameGenerationBackend = upscale::FrameGenerationMode::Auto;
  std::unique_ptr<FrameGeneration> m_frameGeneration;
  // An FSR object being made on the worker. Obsolete ones (the swapchain or
  // the request changed) may still be inside a driver call and are kept until
  // their job ends; they were never used by the GPU.
  struct PreparedFrameGeneration {
    pipeline::PreparationQueue::Ticket job;
    std::unique_ptr<FrameGeneration> generation;
    std::string key;
  };
  std::shared_ptr<PreparedFrameGeneration> m_frameGenerationPreparation;
  std::vector<std::shared_ptr<PreparedFrameGeneration>> m_obsoleteFrameGeneration;
  uint64_t m_frameGenerationGeneration = 0;
  std::string m_frameGenerationStatus;
  uint64_t m_generatedFrames = 0, m_duplicatedFrames = 0;
  // Rendered frames presented with fewer generated frames than asked for (no
  // free swapchain image in time), and the swapchain's present mode: what a
  // report of "no generated frames" needs first.
  uint64_t m_ungeneratedFrames = 0;
  VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
  // The log's state: what was last written, the result of the last failed
  // acquire of a generated frame's image, and how late the presenter thread's
  // presents were at worst since the last line (microseconds).
  FILE *m_frameGenerationLog = nullptr;
  bool m_frameGenerationLogHeader = false;
  std::string m_loggedFrameGenerationStatus;
  std::chrono::steady_clock::time_point m_frameGenerationLogStart{}, m_frameGenerationLogged{};
  uint64_t m_loggedGenerated = 0, m_loggedDuplicated = 0, m_loggedUngenerated = 0, m_loggedPresentations = 0;
  int m_lastGeneratedAcquire = 0;
  std::atomic<int64_t> m_presentLateMicroseconds{0};
  // How long generated and rendered frames stayed up before the next present
  // replaced them (sums since the last line; the presenter thread's clock).
  std::atomic<int64_t> m_generatedShownMicroseconds{0}, m_renderedShownMicroseconds{0};
  std::atomic<uint32_t> m_generatedShownCount{0}, m_renderedShownCount{0};
  std::chrono::steady_clock::time_point m_renderedShownSince{}; // presenter thread only
  // Since the last line, in microseconds (min, max; sum and count where an
  // average is logged): how long each kind of frame stayed up, the host's
  // spacing of rendered frames, the GPU's time for a frame (submission to
  // completion) and the wait for the previous frame's images in presentImages.
  struct LogRange {
    std::atomic<int64_t> low{INT64_MAX}, high{0}, sum{0}, change{0}, last{-1};
    std::atomic<uint32_t> count{0};
    void add(int64_t value) {
      if (const auto before = last.exchange(value); before >= 0)
        change += value > before ? value - before : before - value;
      for (auto v = low.load(); value < v && !low.compare_exchange_weak(v, value);) {
      }
      for (auto v = high.load(); value > v && !high.compare_exchange_weak(v, value);) {
      }
      sum += value;
      ++count;
    }
    // "min/average/max ~change" in ms (change: from one to the next, on
    // average: how unevenly they come); resets.
    std::string take() {
      const auto n = count.exchange(0);
      const double l = double(low.exchange(INT64_MAX)) / 1000, h = double(high.exchange(0)) / 1000,
                   a = double(sum.exchange(0)) / 1000 / (n ? n : 1),
                   c = double(change.exchange(0)) / 1000 / (n ? n : 1);
      char text[80];
      std::snprintf(text, sizeof(text), "%.1f/%.1f/%.1f ~%.1f", n ? l : 0., a, h, c);
      return text;
    }
  } m_logGenerated, m_logRendered, m_logHost, m_logGpu, m_logWait;
  bool m_lowLatencySupported = false, m_lowLatencyRequested = false, m_lowLatencyActive = false;
  // VK_NV_present_metering: a frame's images are presented together and the
  // driver shows them evenly spaced (presentImages), instead of the presenter
  // thread's timed presents. m_meteringRefused: presents whose configuration
  // the driver did not take (those frames are timed here), since the last line.
  // Its structure types by value: the Vulkan headers declare them only with
  // VK_ENABLE_BETA_EXTENSIONS.
  static constexpr auto presentConfigType = VkStructureType(1000613000);
  static constexpr auto presentMeteringFeaturesType = VkStructureType(1000613001);
  bool m_presentMeteringSupported = false;
  std::atomic<uint32_t> m_meteredFrames{0}, m_meteringRefused{0};
  PFN_vkSetLatencySleepModeNV m_setLatencySleepMode = nullptr;
  PFN_vkLatencySleepNV m_latencySleep = nullptr;
  PFN_vkSetLatencyMarkerNV m_setLatencyMarker = nullptr;
  VkSemaphore m_latencySemaphore = VK_NULL_HANDLE; // timeline: the driver signals the end of a sleep
  uint64_t m_latencyValue = 0;
  double m_latencySleepMilliseconds = 0;
  // The host's recent interval between sleeps (smoothed), which bounds one.
  double m_latencyIntervalMilliseconds = 0;
  std::chrono::steady_clock::time_point m_latencySleptAt{};
  // The driver's sleep runs on a thread of its own, so that the host waits
  // for it only as long as a sleep is worth (vkLatencySleepNV itself blocks,
  // now and then for four frames' time).
  struct LatencySleeper {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake, done;
    std::function<void()> job;
    bool busy = false, stop = false;
    // Set while the driver's sleep call is in progress, for latencyMarker:
    // the driver makes a marker wait for it.
    std::atomic<bool> sleeping{false};
    // Runs work on the thread and waits for it at most milliseconds; while
    // an earlier sleep is still running, waits for that one instead.
    void run(std::function<void()> work, double milliseconds);
    void idle();     // returns once no sleep is running
    void shutdown(); // idle, and the thread joined
    ~LatencySleeper() { shutdown(); }
  } m_latencySleeper;
  WaitedFrame m_waitedFrame; // caller-owned: taken while the render thread is idle
  double m_queueLockMilliseconds = 0; // render-owned: the frame's wait for the queue mutex
  // The frame the host simulates (caller's thread) and the one being
  // rendered (copied at Present, read by the render thread).
  uint64_t m_latencyFrame = 0, m_latencyRenderFrame = 0;
  // The presents of frames with generated ones: a job a frame on its own
  // thread, at most two unfinished (the host is not held for a frame's images
  // to have been shown, only for those of the frame before it).
  // finishDeferredPresent waits for them all. m_paceClock: when the last
  // frame's first image was shown (presenter thread only).
  PresentQueue m_presenter;
  std::chrono::steady_clock::time_point m_paceClock{};
  std::recursive_mutex m_queueMutex;
  std::atomic<int> m_deferredResult{VK_SUCCESS};
  std::chrono::steady_clock::time_point m_lastRenderedPresent{};
  double m_renderedInterval = 0; // smoothed milliseconds between rendered frames
  remixapi_UIState m_uiState = REMIXAPI_UI_STATE_NONE;
  float m_clearColor[4] = {0.025f, 0.035f, 0.055f, 1.0f};
  char m_input[256] = {};
  ImGuiContext *m_gui = nullptr;
  bool m_uiFrameBuilt = false; // drawUi(VK_NULL_HANDLE) completed an ImGui frame
  VkPhysicalDeviceProperties m_properties = {};
  VkInstance m_instance = VK_NULL_HANDLE;
  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueue m_queue = VK_NULL_HANDLE;
  uint32_t m_queueFamily = VK_QUEUE_FAMILY_IGNORED;
  bool m_headless = false, m_disableProfiles = false, m_disableMods = false;
  VkImage m_sharedImage = VK_NULL_HANDLE;
  VkDeviceMemory m_sharedMemory = VK_NULL_HANDLE;
  VkSemaphore m_sharedReady = VK_NULL_HANDLE, m_sharedReleased = VK_NULL_HANDLE;
  uint64_t m_sharedSize = 0;
  uint32_t m_sharedMemoryType = 0;
  bool m_sharedPresented = false;
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
  // HDR10 output (runtime/hdr_output.h): active while the swapchain is
  // ST.2084. m_hdr10Requested: the swapchain was asked for it, which a change
  // of wantsHdr10() recreates; m_hdrStatus: why it is inactive.
  HdrOutput m_hdrOutput;
  bool m_swapchainColorSpace = false, m_hdr10Requested = false, m_swapchainHdr10 = false;
  std::string m_hdrStatus;
  VkExtent2D m_swapchainExtent = {};
  std::vector<VkImage> m_swapchainImages;
  std::vector<VkImageView> m_imageViews;
  std::vector<VkFramebuffer> m_framebuffers;
  VkRenderPass m_renderPass = VK_NULL_HANDLE;
  VkRenderPass m_overlayPass = VK_NULL_HANDLE;
  VkRenderPass m_resumePass = VK_NULL_HANDLE; // loads colour and depth (halo outlines)
  VkCommandPool m_commandPool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> m_commandBuffers;
  struct FrameSync {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore generatedAvailable[FrameGeneration::maxGenerated] = {}; // frame generation's images
    VkFence presented = VK_NULL_HANDLE; // signalled when the frame's GPU work is done (the presenter waits for it)
    VkFence fence = VK_NULL_HANDLE;
    std::shared_ptr<FrameSnapshot> snapshot;
  } m_frames[gpuFrameCount];
  uint32_t m_frameIndex = 0, m_frameCount = gpuFrameCount;
  // GPU pass timers: render-owned recorder; the setting is copied at Present
  // setup (applySettings) for the render thread.
  gpu::GpuTimers m_gpuTimers;
  bool m_gpuTimersEnabled = false;
  uint64_t m_recordedFrames = 0;
  // Presentation may still consume a semaphore after the submission fence
  // signals.
  std::vector<VkSemaphore> m_renderFinished;
};
} // namespace remix
