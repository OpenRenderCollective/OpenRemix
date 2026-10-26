// The Metal runtime: lifecycle, configuration and statistics (runtime/runtime.cpp
// and runtime/configuration.cpp's counterparts).
#include "metal/reconstruction.h"
#include "metal/runtime_gpu.h"
#include <SDL3/SDL.h>
#include <remix/remix_editor_ext.h>
#include "api/exports.h"
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace remix {
  RemixNativeRuntime::RemixNativeRuntime() = default;

  namespace metal {
    void abandon(std::shared_ptr<PreparedTracer> &preparation);
  } // namespace metal

  RemixNativeRuntime::~RemixNativeRuntime() {
    // Destroyed without Shutdown: the process is exiting (static
    // destruction). Apple's frameworks have torn down their own statics by
    // now (releasing a MetalFX denoised scaler aborts in MPSGraph), so the
    // GPU state is left to the process's end instead of released.
    if (m_gpu) {
      metal::abandon(m_preparation);
      for (auto &preparation : m_obsoletePreparations)
        metal::abandon(preparation);
      (void)m_pathTracer.release();
      (void)m_gpu.release();
      return;
    }
    m_window.shutdown();
  }

  remixapi_ErrorCode RemixNativeRuntime::fail(const char *reason) {
    m_failed = true;
    m_failure = reason;
    SDL_Log("Remix Metal failure: %s", reason);
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  remixapi_ErrorCode RemixNativeRuntime::initialize(const remixapi_StartupInfo &info) {
    if (isInitialized())
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    m_disableProfiles = m_disableMods = false;
    struct Extension {
      remixapi_StructType type;
      const void *next;
    };
    for (auto p = static_cast<const Extension *>(info.pNext); p; p = static_cast<const Extension *>(p->next)) {
      if (p->type != REMIXAPI_STRUCT_TYPE_EDITOR_STARTUP_EXT)
        continue;
      const auto &editor = *reinterpret_cast<const remixapi_EditorStartupEXT *>(p);
      if (editor.version != 1)
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      // The headless editor shares Vulkan images; Metal has no such target.
      if (editor.headless)
        return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
      m_disableProfiles = editor.disableProfiles;
      m_disableMods = editor.disableMods;
    }
    m_failed = false;
    m_failure.clear();
    m_presentationCount = 0;
    m_lastPresentation = {};
    m_longestPresentationGap = 0;
    m_framePerformance = {.structSize = sizeof(remixapi_FramePerformanceEXT)};
    // info.disableSrgbConversionForOutput is accepted but ignored: the output
    // is always gamma 2.2 encoded (UNORM drawable).
    @autoreleasepool {
      try {
        if (!m_disableProfiles)
          m_profiles.startup();
        m_profileUi = {};
        m_uiState = REMIXAPI_UI_STATE_NONE;
        if (!m_profiles.message().empty())
          SDL_Log("OpenRemix profiles: %s", m_profiles.message().c_str());
        auto status = m_window.initialize(info);
        if (status != REMIXAPI_ERROR_CODE_SUCCESS)
          return status;
        auto finish = [&](remixapi_ErrorCode error) {
          if (error != REMIXAPI_ERROR_CODE_SUCCESS)
            shutdown();
          return error;
        };
        m_gpu = std::make_unique<Gpu>();
        auto &g = *m_gpu;
        g.device = MTLCreateSystemDefaultDevice();
        if (!g.device) {
          SDL_Log("Remix Metal: no Metal device");
          return finish(REMIXAPI_ERROR_CODE_GENERAL_FAILURE);
        }
        g.queue = [g.device newCommandQueue];
        g.layer = (__bridge CAMetalLayer *)m_window.metalLayer();
        if (!g.queue || !g.layer) {
          SDL_Log("Remix Metal: %s", m_window.lastError().c_str());
          return finish(REMIXAPI_ERROR_CODE_GENERAL_FAILURE);
        }
        const char *frames = std::getenv("REMIX_FRAMES_IN_FLIGHT");
        g.frameCount = frames && std::strcmp(frames, "1") == 0 ? 1u : gpuFrameCount;
        g.frames = dispatch_semaphore_create(g.frameCount);
        g.layer.device = g.device;
        // UNORM output like the Vulkan swapchain: the shaders encode gamma 2.2.
        g.format = MTLPixelFormatBGRA8Unorm;
        g.layer.pixelFormat = g.format;
        // Screenshots copy the drawable.
        g.layer.framebufferOnly = NO;
        g.layer.maximumDrawableCount = 3;
        g.timers.initialize(g.device);
        m_traceSupported = g.device.supportsRaytracing && [g.device supportsFamily:MTLGPUFamilyApple7];
        m_traceUnavailable = m_traceSupported ? "" : "The Metal device does not support ray tracing (Apple7+)";
        SDL_Log("OpenRemix Metal device: %s (ray tracing %s)", g.device.name.UTF8String,
                m_traceSupported ? "supported" : "unsupported");
        applySettings();
        m_raster.initializeGpu((__bridge void *)g.device);
        if (!initializeUi())
          return finish(REMIXAPI_ERROR_CODE_GENERAL_FAILURE);
        m_initialized = true;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      } catch (const std::exception &error) {
        SDL_Log("Remix Metal startup failed: %s", error.what());
        shutdown();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      } catch (...) {
        shutdown();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
    }
  }

  namespace metal {
    // A path tracer being prepared on its own thread (the Vulkan runtime's
    // PreparedConfiguration): pipeline compilation takes seconds on Metal.
    struct PreparedTracer {
      PathTracer::Configuration configuration;
      std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
      std::unique_ptr<PathTracer> tracer;
      std::string failure;
      std::atomic<bool> done{false};
      std::thread thread;
      ~PreparedTracer() {
        if (thread.joinable())
          thread.join();
      }
    };

    // A preparation still compiling is not waited for (a Metal compile
    // cannot be cancelled, and a cold one takes a minute): its thread is
    // detached and keeps its state, which is never freed. The pipeline cache
    // it compiles into outlives static destruction (shaders.mm).
    void abandon(std::shared_ptr<PreparedTracer> &preparation) {
      if (preparation && !preparation->done && preparation->thread.joinable()) {
        preparation->thread.detach();
        new std::shared_ptr<PreparedTracer>(std::move(preparation));
      }
      preparation.reset();
    }
  } // namespace metal

  void RemixNativeRuntime::destroyMetal() {
    if (!m_gpu)
      return;
    @autoreleasepool {
      auto &g = *m_gpu;
      // Every submitted frame completes before its resources are released,
      // and is presented (frame generation presents from its own queue).
      if (g.queue) {
        id<MTLCommandBuffer> drain = [g.queue commandBuffer];
        [drain commit];
        [drain waitUntilCompleted];
      }
      if (g.frames) {
        uint32_t held = 0;
        while (held < g.frameCount &&
               !dispatch_semaphore_wait(g.frames, dispatch_time(DISPATCH_TIME_NOW, int64_t(2e9))))
          ++held;
        for (; held; --held)
          dispatch_semaphore_signal(g.frames);
      }
      completeScreenshot(true);
      if (m_presentationCount)
        SDL_Log("OpenRemix Metal: presentations=%llu longest_presentation_gap_ms=%.3f",
                (unsigned long long)m_presentationCount, m_longestPresentationGap);
      shutdownUi();
      metal::abandon(m_preparation);
      for (auto &preparation : m_obsoletePreparations)
        metal::abandon(preparation);
      m_obsoletePreparations.clear();
      m_pathTracer.reset();
      m_localSourcesSeen = m_loading = false;
      m_failedConfiguration.reset();
      m_raster.shutdownGpu();
      m_gpu.reset();
    }
  }

  remixapi_ErrorCode RemixNativeRuntime::shutdown() {
    if (m_window.handle()) {
      remixapi_WindowState state = {};
      if (m_window.getWindowState(state) == REMIXAPI_ERROR_CODE_WRONG_THREAD)
        return REMIXAPI_ERROR_CODE_WRONG_THREAD;
    }
    destroyMetal();
    m_window.shutdown();
    m_raster.reset();
    m_initialized = false;
    m_frameStarted = false;
    m_failed = false;
    m_vsync = false;
    m_backend = 0;
    m_traceOptions = {};
    if (!m_disableProfiles)
      m_profiles.savePanel(m_profiles.panel());
    m_profiles = {};
    m_appliedSettings = {};
    m_profileUi = {};
    m_sharcReset = 0;
    const auto defaults = config::Settings{};
    std::copy(defaults.clearColor.begin(), defaults.clearColor.end(), m_clearColor);
    m_traceSupported = false;
    m_uiState = REMIXAPI_UI_STATE_NONE;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::selectBackend(const char *value) {
    auto next = m_profiles.current();
    if (config::set(next, "rtx.native.backend", value) != config::SetResult::Success)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (next.backend && !m_traceSupported) {
      SDL_Log("Path tracing unavailable: %s", m_traceUnavailable.c_str());
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
    m_profiles.edit(next);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::configureSetting(const char *key, const char *value) {
    auto next = m_profiles.current();
    if (config::set(next, key, value) != config::SetResult::Success || !m_profiles.edit(next))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    m_raster.setKeepCompressedTextures(m_profiles.current().compressedTextures);
    m_raster.setRefreshGeometry(m_profiles.current().refreshGeometry);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // runtime/configuration.cpp configureDefault.
  remixapi_ErrorCode RemixNativeRuntime::configureDefault(const char *key, const char *value) {
    if (!m_profiles.setDefault(key, value))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    const auto &settings = m_profiles.current();
    if (settings.backend && !m_traceSupported) {
      // A path-tracing default where it cannot run: the raster backend stays.
      auto next = settings;
      next.backend = 0;
      m_profiles.edit(next);
    }
    m_raster.setKeepCompressedTextures(m_profiles.current().compressedTextures);
    m_raster.setRefreshGeometry(m_profiles.current().refreshGeometry);
    m_hostPasses.setEnabled(m_profiles.current().hostPasses);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  std::string RemixNativeRuntime::pipelineReport() const { return m_gpu ? metal::pipelineReport() : "no device\n"; }

  remixapi_ErrorCode RemixNativeRuntime::configureTracing(const char *key, const char *value) {
    if (!std::strcmp(key, "sharcReset")) {
      unsigned number = 0;
      const auto end = value + std::strlen(value);
      auto parsed = std::from_chars(value, end, number);
      if (std::strcmp(value, "true") && (parsed.ec != std::errc{} || parsed.ptr != end || number != 1))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      ++m_sharcReset;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    return configureSetting((std::string("rtx.native.pathtrace.") + key).c_str(), value);
  }

  void RemixNativeRuntime::applySettings() {
    const auto &settings = m_profiles.current();
    m_raster.setKeepCompressedTextures(settings.compressedTextures);
    m_raster.setRefreshGeometry(settings.refreshGeometry);
    m_vsync = settings.vsync;
  }


  bool RemixNativeRuntime::preparationPending() const { return bool(m_preparation); }
  double RemixNativeRuntime::preparationSeconds() const {
    return m_preparation ? std::chrono::duration<double>(std::chrono::steady_clock::now() - m_preparation->started).count()
                         : 0;
  }

  // runtime/configuration.cpp prepareSettings: a configuration change prepares
  // a new tracer while the active one (or raster) keeps presenting.
  void RemixNativeRuntime::prepareSettings(uint32_t width, uint32_t height) {
    const auto &settings = m_profiles.current();
    auto &g = *m_gpu;
    // The sticky local-light contract: temporary culling must not rebuild
    // denoisers or discard their histories.
    const bool detectedLocal =
        m_raster.hasSphereLights() || m_raster.staticEmissiveCount() ||
        std::any_of(m_raster.draws().begin(), m_raster.draws().end(), [](const auto &draw) {
          const auto &e = draw.material->emission;
          return draw.traced() && (e[0] > 0 || e[1] > 0 || e[2] > 0);
        });
    m_localSourcesSeen = settings.backend && (m_localSourcesSeen || detectedLocal);
    const bool local = m_localSourcesSeen || (settings.trace.sigma && settings.trace.localLights);
    std::erase_if(m_obsoletePreparations, [](const auto &preparation) { return preparation->done.load(); });
    auto retire = [&] {
      if (m_preparation)
        m_obsoletePreparations.push_back(std::move(m_preparation));
    };
    if (!settings.backend || !m_traceSupported) {
      retire();
      if (m_pathTracer) {
        id<MTLCommandBuffer> drain = [g.queue commandBuffer];
        [drain commit];
        [drain waitUntilCompleted];
        m_pathTracer.reset();
      }
      m_failedConfiguration.reset();
    } else {
      const bool perspective = m_raster.cameraValid() && metal::reconstruction::supportsCamera(m_raster.projection());
      const auto desired = metal::PathTracer::Configuration::of(
          (__bridge void *)g.device, settings.trace, settings.frameGeneration, local, perspective, width, height,
          uint32_t(g.format));
      if (m_pathTracer && m_pathTracer->configuration() == desired)
        retire();
      else {
        if (m_preparation && !(m_preparation->configuration == desired))
          retire();
        if (!m_preparation && m_failedConfiguration != desired) {
          auto next = std::make_shared<metal::PreparedTracer>();
          next->configuration = desired;
          id<MTLDevice> device = g.device;
          auto *prepared = next.get();
          next->thread = std::thread([prepared, device] {
            @autoreleasepool {
              try {
                auto tracer = std::make_unique<metal::PathTracer>();
                tracer->initialize((__bridge void *)device, prepared->configuration);
                prepared->tracer = std::move(tracer);
              } catch (const std::exception &error) {
                prepared->failure = error.what();
              } catch (...) {
                prepared->failure = "Path tracer preparation failed";
              }
              prepared->done = true;
            }
          });
          m_preparation = std::move(next);
          ++m_preparationGeneration;
        }
        if (m_preparation && m_preparation->done) {
          auto prepared = std::move(m_preparation);
          if (prepared->thread.joinable())
            prepared->thread.join();
          if (!prepared->failure.empty()) {
            m_failedConfiguration = prepared->configuration;
            m_traceUnavailable = prepared->failure;
            SDL_Log("Path tracing configuration failed: %s", prepared->failure.c_str());
          } else {
            // The replaced tracer's frames complete before it is released.
            id<MTLCommandBuffer> drain = [g.queue commandBuffer];
            [drain commit];
            [drain waitUntilCompleted];
            m_pathTracer = std::move(prepared->tracer);
            m_traceUnavailable.clear();
            const auto &c = m_pathTracer->configuration();
            SDL_Log("OpenRemix Metal path tracer ready in %.1f ms (%ux%u, REBLUR %d, TAA %d, SIGMA %d, local %d, "
                    "SHARC %d)",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepared->started)
                        .count(),
                    c.width, c.height, c.reblur, c.taa, c.sigma, c.localDenoise, c.sharc);
          }
        }
      }
    }
    // A tracer prepared for another size cannot render into this one.
    const bool sized = m_pathTracer && m_pathTracer->configuration().displayWidth == width &&
                       m_pathTracer->configuration().displayHeight == height;
    m_backend = settings.backend && sized ? 1 : 0;
    m_loading = settings.backend && m_traceSupported && !m_backend;
    m_appliedSettings = settings;
    m_traceOptions = settings.trace;
    m_traceOptions.sharcReset = m_sharcReset;
    std::copy(settings.clearColor.begin(), settings.clearColor.end(), m_clearColor);
  }

  metal::ReconstructionStatistics RemixNativeRuntime::reconstructionStatistics() const {
    metal::ReconstructionStatistics result;
    result.requested = m_profiles.current().trace.upscaler;
    std::string reason;
    result.srSupported = m_gpu && metal::reconstruction::upscalingSupported(m_gpu->device, reason);
    result.rrSupported = m_gpu && metal::reconstruction::denoisingSupported(m_gpu->device, reason);
    result.sdkAvailable = result.srSupported;
    if (m_gpu)
      result.render = result.display = {m_gpu->width, m_gpu->height};
    if (m_pathTracer && m_backend) {
      auto s = m_pathTracer->upscalingStatistics();
      s.requested = result.requested;
      s.srSupported = result.srSupported;
      s.rrSupported = result.rrSupported;
      s.sdkAvailable = result.sdkAvailable;
      result = std::move(s);
    }
    return result;
  }

  remixapi_BackendCapabilitiesEXT RemixNativeRuntime::backendCapabilities() const {
    remixapi_BackendCapabilitiesEXT result = {.structSize = sizeof(result),
                                              .supportedBackends = m_traceSupported ? 3u : 1u,
                                              .activeBackend = m_backend,
                                              .requestedBackend = m_profiles.current().backend};
    std::strncpy(result.unavailableReason, m_traceUnavailable.c_str(), sizeof(result.unavailableReason) - 1);
    return result;
  }

  remixapi_TracingStatisticsEXT RemixNativeRuntime::tracingStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->statistics()
                               : remixapi_TracingStatisticsEXT{.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    const bool denoiserWarming = m_pathTracer && m_pathTracer->denoisingStatistics().redrawRequested;
    result.redrawRequested = preparationPending() ||
                             (m_backend && (result.redrawRequested || denoiserWarming ||
                                            m_profiles.current().trace.accumulate)) ||
                             m_profiles.current() != m_appliedSettings;
    return result;
  }

  remixapi_SharcStatisticsEXT RemixNativeRuntime::sharcStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->sharcStatistics()
                               : remixapi_SharcStatisticsEXT{.structSize = sizeof(remixapi_SharcStatisticsEXT)};
    result.requested = m_profiles.current().trace.sharc;
    if (!m_pathTracer)
      std::strncpy(result.unavailableReason, REMIX_HAS_SHARC ? "Path tracing is inactive" : "SHARC was disabled at build time",
                   sizeof(result.unavailableReason) - 1);
    return result;
  }

  remixapi_LocalLightGuidingStatisticsEXT RemixNativeRuntime::localLightGuidingStatistics() const {
    remixapi_LocalLightGuidingStatisticsEXT result =
        m_pathTracer && m_backend ? m_pathTracer->localLightGuidingStatistics()
                                  : remixapi_LocalLightGuidingStatisticsEXT{.structSize = sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
    result.requested = m_profiles.current().trace.localGuiding;
    result.influenceRange = m_profiles.current().trace.localRange;
    return result;
  }

  remixapi_LocalLightStatisticsEXT RemixNativeRuntime::localLightStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->localLightStatistics()
                               : remixapi_LocalLightStatisticsEXT{.structSize = sizeof(remixapi_LocalLightStatisticsEXT)};
    result.enabled = m_profiles.current().trace.localLights;
    result.samples = m_profiles.current().trace.localSamples;
    return result;
  }

  remixapi_ShadowStatisticsEXT RemixNativeRuntime::shadowStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->shadowStatistics()
                               : remixapi_ShadowStatisticsEXT{.structSize = sizeof(remixapi_ShadowStatisticsEXT)};
    result.requested = m_profiles.current().trace.sigma;
    return result;
  }

  remixapi_DenoisingStatisticsEXT RemixNativeRuntime::denoisingStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->denoisingStatistics()
                               : remixapi_DenoisingStatisticsEXT{.structSize = sizeof(remixapi_DenoisingStatisticsEXT)};
    result.requestedDenoiser = m_profiles.current().trace.denoiser;
    result.requestedTaa = m_profiles.current().trace.taa;
    return result;
  }

  remixapi_ErrorCode RemixNativeRuntime::vramStatistics(remixapi_VramStats &stats) const {
    stats = {};
    if (!m_initialized)
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    if (!m_profiles.current().vramStatistics)
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    const auto raster = m_raster.memoryStatistics();
    stats.usedMaterialTextureBytes = raster.textureBytes;
    stats.forkTextureCacheCount = raster.textureCount;
    stats.usedBufferBytes = raster.geometryBytes;
    if (m_pathTracer)
      stats.usedAccelerationStructureBytes = m_pathTracer->statistics().accelerationBytes;
    stats.totalUsedBytes = stats.usedMaterialTextureBytes + stats.usedBufferBytes +
                           stats.usedAccelerationStructureBytes;
    stats.totalAllocatedBytes = stats.totalUsedBytes;
    @autoreleasepool {
      // Unified memory: the process's GPU allocations and the working set the
      // device recommends.
      stats.driverAllocatedBytes = m_gpu->device.currentAllocatedSize;
      stats.driverBudgetBytes = m_gpu->device.recommendedMaxWorkingSetSize;
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }
} // namespace remix

// The headless editor's shared frames are Linux/Vulkan only (runtime/shared_frame.cpp).
extern "C" REMIXAPI int REMIXAPI_CALL remixapi_EditorConfigureSharedFrame(remixapi_EditorSharedFrame *frame) {
  (void)frame;
  return -1;
}
