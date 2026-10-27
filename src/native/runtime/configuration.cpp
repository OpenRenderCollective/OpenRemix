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

#include "runtime/runtime.h"
#include "upscale/camera.h"
#include "upscale/ngx.h"

#include <cstdio>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <charconv>
#include <cmath>

namespace remix {
#if REMIX_HAS_USD
  // Retained BC blocks are read only by USD captures.
  constexpr bool kCaptures = true;
#else
  constexpr bool kCaptures = false;
#endif
  remixapi_ErrorCode RemixNativeRuntime::selectBackend(const char *value) {
    auto next = m_profiles.current();
    if (config::set(next, "rtx.native.backend", value) != config::SetResult::Success)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (next.backend && !m_traceSupported) {
      SDL_Log("Path tracing unavailable: %s", m_traceUnavailable.c_str());
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
    m_backendFailed = false;
    m_profiles.edit(next);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::configureSetting(const char *key, const char *value) {
    auto next = m_profiles.current();
    if (config::set(next, key, value) != config::SetResult::Success || !m_profiles.edit(next))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    // Texture creation needs no frame boundary: later CreateTexture calls see it.
    m_raster.setKeepCompressedTextures(m_profiles.current().compressedTextures);
    scene::retainCompressedUploads = kCaptures && m_profiles.current().captureRetainTextureData;
    m_raster.setRefreshGeometry(m_profiles.current().refreshGeometry);
    // The host's next submission already sees it (else from the next frame).
    m_hostPasses.setEnabled(m_profiles.current().hostPasses);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

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
    scene::retainCompressedUploads = kCaptures && m_profiles.current().captureRetainTextureData;
    m_raster.setRefreshGeometry(m_profiles.current().refreshGeometry);
    m_hostPasses.setEnabled(m_profiles.current().hostPasses);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  namespace {
  std::string resourceKey(const config::Settings &settings, VkExtent2D extent,
                          bool perspective, bool local) {
    // Only changes to resource/pipeline contracts replace the renderer.
    // Continuous lighting and display edits keep their temporal state.
    config::Settings resources;
    resources.backend = settings.backend;
    auto &out = resources.trace;
    const auto &in = settings.trace;
    out.accumulate = in.accumulate;
    out.validation = in.validation;
    out.denoiser = in.denoiser;
    out.taa = in.taa;
    out.sigma = in.sigma;
    out.localDenoiser = in.localDenoiser;
    out.separateLocalLighting = normalized(in).separateLocalLighting;
    out.restir = in.restir;
    out.upscaler = in.upscaler;
    out.dlssQuality = in.dlssQuality;
    out.fsrQuality = in.fsrQuality;
    out.dlssRayReconstruction = in.dlssRayReconstruction;
    out.sharc = in.sharc;
    out.sharcDebug = in.sharcDebug;
    out.sharcCapacityLog2 = in.sharcCapacityLog2;
    out.bounces = in.bounces ? 1 : 0;
    out.rasterPrimary = in.rasterPrimary;
    return config::serialize(resources) + std::to_string(extent.width) + ":" +
           std::to_string(extent.height) + ":" + std::to_string(perspective) +
           ":" + std::to_string(local);
  }
  } // namespace
  bool RemixNativeRuntime::preparationPending() const {
    const auto pipelines =
        m_pipelines ? m_pipelines->statistics() : pipeline::Statistics{};
    return pipelines.completed != pipelines.requested || bool(m_preparation) || bool(m_postPreparation) ||
           m_rasterPreparing || (m_ngxPreparing && m_ngxPreparing->load()) ||
           (m_traceSupported && m_profiles.current().backend && !m_pathTracer && !m_worldCameraHold &&
            m_preparationFailure.empty());
  }
  void RemixNativeRuntime::retireTracer(std::unique_ptr<PathTracer> tracer,
                                        std::shared_ptr<void> depth) {
    if (!tracer)
      return;
    std::shared_ptr<void> sdkGuard;
    const auto backend = tracer->upscalingStatistics().backend;
    if (backend == upscale::Backend::DlssSr ||
        backend == upscale::Backend::DlssRr) {
      auto count = m_ngxPreparing;
      ++*count;
      sdkGuard =
          std::shared_ptr<void>(count.get(), [count](void *) { --*count; });
    }
    auto retired = std::shared_ptr<PathTracer>(std::move(tracer));
    m_pipelines->prepare("retire-" + std::to_string(++m_preparationGeneration),
                         [retired, depth, sdkGuard]() mutable {
                           retired.reset();
                           depth.reset();
                           sdkGuard.reset();
                         });
  }
  void RemixNativeRuntime::applySettings() {
    const auto &settings = m_profiles.current();
    m_requestedBackend = settings.backend;
    m_raster.setKeepCompressedTextures(settings.compressedTextures);
    scene::retainCompressedUploads = kCaptures && settings.captureRetainTextureData;
    m_raster.setRefreshGeometry(settings.refreshGeometry);
    m_raster.syncHiddenTextures(settings.hideTextures);
    // Read by the frame's recording (the render thread with asynchronous presentation).
    m_gpuTimersEnabled = settings.gpuPassTimers;
    // Startup needs the requested presenter settings before there is a target.
    // Tracing settings activate in prepareSettings only after preflight
    // succeeds.
    m_vsync = settings.vsync;
    m_lowLatencyRequested = m_lowLatencySupported && !m_headless && (settings.lowLatency || settings.frameGeneration);
    m_frameGenerationRequested = settings.frameGeneration && !m_headless
                                     ? std::clamp(settings.frameGenerationFrames, 1u, FrameGeneration::maxGenerated)
                                     : 0;
    // Takes effect in prepareFrameGeneration, through a new swapchain.
    m_frameGenerationBackend = settings.frameGenerationBackend;
    if (!m_initialized || !settings.backend || !m_traceSupported) {
      if (!m_initialized)
        m_appliedSettings = settings;
      std::copy(settings.clearColor.begin(), settings.clearColor.end(),
                m_clearColor);
    }
  }
  void RemixNativeRuntime::prepareSettings() {
    const auto &settings = m_profiles.current();
    m_hostPasses.setEnabled(settings.hostPasses);
    const auto ngxCapabilities = m_ngx->capabilities();
    const bool needsNgx = settings.trace.upscaler != upscale::Mode::Off &&
                          settings.trace.upscaler != upscale::Mode::Fsr31 &&
                          ngxCapabilities.sdk &&
                          (!ngxCapabilities.initializationComplete ||
                           ngxCapabilities.sr || ngxCapabilities.rr);
    // This frame: the API's scene, or the published frame with asynchronous
    // presentation (published before settings are prepared).
    const auto &frame = renderer();
    const bool perspective = frame.cameraValid() &&
                             upscale::supportsCamera(frame.projection());
    const bool detectedLocal =
        frame.hasSphereLights() || frame.staticEmissiveCount() ||
        std::any_of(frame.draws().begin(), frame.draws().end(),
                    [](const auto &draw) {
                      const auto &e = draw.material->emission;
                      return draw.traced() &&
                             (e[0] > 0 || e[1] > 0 || e[2] > 0);
                    });
    // Match the tracer's sticky local-light contract: temporary culling must
    // not repeatedly rebuild denoisers or discard their histories.
    m_localSourcesSeen =
        settings.backend && (m_localSourcesSeen || detectedLocal);
    // SIGMA's sun-only composition multiplies the whole direct signal by its
    // shadow mask. Reserve a separate local signal when local lighting is
    // enabled, so a newly appearing torch is never treated as sunlight while
    // a scene-dependent optimization is being prepared.
    const bool local = m_localSourcesSeen ||
                       (settings.trace.sigma && settings.trace.localLights);
    // A menu or loading screen has no perspective camera. A renderer prepared
    // for it runs without the upscaler (NRD denoising in place of DLSS-RR)
    // and is replaced as soon as the world appears, its compiles having held
    // the workers meanwhile. Hold the first preparation for the world's
    // camera, within a bound.
    const auto now = std::chrono::steady_clock::now();
    const bool wantsWorldCamera = !m_pathTracer && !m_preparation && settings.backend && m_traceSupported &&
                                  settings.trace.upscaler != upscale::Mode::Off && !perspective;
    if (!wantsWorldCamera)
      m_worldCameraWait = {};
    else if (m_worldCameraWait == std::chrono::steady_clock::time_point{})
      m_worldCameraWait = now;
    const bool holding = wantsWorldCamera && now - m_worldCameraWait < std::chrono::seconds(120);
    // Not loading while held: the raster backend presents the menu as it is.
    m_worldCameraHold = holding;
    const auto requestedKey =
        resourceKey(settings, m_swapchainExtent, perspective, local);
    if (requestedKey != m_reconstructionFailureKey) {
      m_reconstructionFailureMask = 0;
      m_reconstructionFailure.clear();
      m_reconstructionFailureKey = requestedKey;
    }
    if (m_pathTracer) {
      const auto reconstruction = m_pathTracer->upscalingStatistics();
      if (reconstruction.failed &&
          requestedKey == resourceKey(m_appliedSettings, m_swapchainExtent,
                                      perspective, local)) {
        m_reconstructionFailureMask |= 1u << uint32_t(reconstruction.backend);
        m_reconstructionFailure = reconstruction.failure;
      }
    }
    const auto key = requestedKey +
                     ":fallback:" + std::to_string(m_reconstructionFailureMask);
    // Suppress automatic retries only while the failed request remains
    // selected. Leaving it and returning is an explicit new attempt.
    if (key != m_failedPreparationKey)
      m_failedPreparationKey.clear();
    const auto revision = m_configurationRevision.request(key);
    auto retire = [&] {
      if (!m_preparation)
        return;
      m_pipelines->cancel(m_preparation->job);
      m_obsoletePreparations.push_back(std::move(m_preparation));
    };
    // A submitted NGX initialization owns GPU resources even if superseded.
    // Retire it only after its fence, on this same queue thread.
    for (auto it = m_obsoletePreparations.begin();
         it != m_obsoletePreparations.end();) {
      auto &old = **it;
      if (!old.job->ready()) {
        ++it;
        continue;
      }
      if (old.tracer && !old.tracer->finishPreparation(m_queue)) {
        ++it;
        continue;
      }
      retireTracer(std::move(old.tracer), old.depth);
      it = m_obsoletePreparations.erase(it);
    }
    if (!settings.backend || !m_traceSupported) {
      retire();
      if (m_pathTracer) {
        const auto status = vkDeviceWaitIdle(m_device);
        if (status != VK_SUCCESS)
          throw status;
        retireTracer(std::move(m_pathTracer), renderer().depthLifetime());
      }
      m_activePreparationKey.clear();
      m_backend = m_targetBackend = 0;
      m_appliedSettings = settings;
    } else {
      if (m_preparation && m_preparation->key != key)
        retire();
      if (key != m_activePreparationKey && key != m_failedPreparationKey &&
          !m_preparation && !holding) {
        auto next = std::make_shared<PreparedConfiguration>();
        next->key = key;
        next->generation = ++m_preparationGeneration;
        next->revision = revision;
        next->depth = renderer().depthLifetime();
        const auto physical = m_physicalDevice;
        const auto device = m_device;
        const auto family = m_queueFamily;
        const auto pass = m_renderPass;
        const auto extent = m_swapchainExtent;
        const auto depthFormat = renderer().depthFormat();
        const auto depth = renderer().depthView();
        const auto options = normalized(settings.trace);
        const auto ngx = m_ngx;
        const auto exposureState = m_exposureState;
        const auto failedBackends = m_reconstructionFailureMask;
        const auto reconstructionFailure = m_reconstructionFailure;
        // Increment on the render thread before enqueuing so no evaluation can
        // race the beginning of a worker's SDK initialization call.
        std::shared_ptr<void> sdkGuard;
        if (needsNgx) {
          auto count = m_ngxPreparing;
          ++*count;
          sdkGuard =
              std::shared_ptr<void>(count.get(), [count](void *) { --*count; });
        }
        next->job = m_pipelines->prepare(std::to_string(next->generation), [=] {
          if (sdkGuard)
            ngx->initializeDevice();
          auto tracer = std::make_unique<PathTracer>();
          tracer->initialize(physical, device, family, true,
                             options.rasterPrimary, exposureState);
          tracer->setNgx(ngx);
          tracer->configureTarget(pass, extent, depthFormat, depth);
          tracer->prepareConfiguration(options, perspective, local,
                                       failedBackends, reconstructionFailure);
          next->tracer = std::move(tracer);
          (void)sdkGuard;
        });
        m_pipelines->note("renderer preparation " +
                          std::to_string(next->generation) + " started");
        m_preparation = std::move(next);
        m_preparationFailure.clear();
      }
      if (m_preparation &&
          m_configurationRevision.accepts(m_preparation->revision) &&
          m_preparation->job->ready()) {
        try {
          m_preparation->job->check();
          if (m_preparation->tracer->finishPreparation(m_queue)) {
            // Raster sky/overlay states are also required by the replacement.
            // Discover them before activation while the old renderer is usable.
            renderer().setSkyTarget(m_preparation->tracer->skyPass());
            const bool rasterReady = renderer().preparePipelines(true);
            renderer().setSkyTarget(m_pathTracer ? m_pathTracer->skyPass()
                                                 : VK_NULL_HANDLE);
            if (!rasterReady)
              return;
            const auto status = vkDeviceWaitIdle(m_device);
            if (status != VK_SUCCESS)
              throw status;
            retireTracer(std::move(m_pathTracer), renderer().depthLifetime());
            m_pathTracer = std::move(m_preparation->tracer);
            m_exposureState = m_pathTracer->exposureState();
            m_activePreparationKey = m_preparation->key;
            const auto statistics = m_pipelines->statistics();
            SDL_Log(
                "OpenRemix preparation: generation=%llu elapsed_ms=%.3f "
                "created=%llu reused=%llu cache_hits=%llu",
                (unsigned long long)m_preparation->generation,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - m_preparation->started)
                    .count(),
                (unsigned long long)statistics.created,
                (unsigned long long)statistics.reused,
                (unsigned long long)statistics.cacheHits);
            char event[160];
            std::snprintf(event, sizeof(event),
                          "renderer preparation %llu active after %.0f ms",
                          (unsigned long long)m_preparation->generation,
                          std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() -
                              m_preparation->started)
                              .count());
            m_pipelines->note(event);
            m_preparation.reset();
            m_backend = m_targetBackend = 1;
            m_traceUnavailable.clear();
          }
        } catch (VkResult result) {
          if (result == VK_ERROR_DEVICE_LOST)
            throw;
          m_preparationFailure = "Configuration preparation failed (Vulkan " +
                                 std::to_string(result) + ")";
        } catch (const std::exception &error) {
          m_preparationFailure = error.what();
        } catch (...) {
          m_preparationFailure = "Configuration preparation failed";
        }
        if (!m_preparationFailure.empty()) {
          m_traceUnavailable = m_preparationFailure;
          m_failedPreparationKey = key;
          retire();
          SDL_Log("%s", m_preparationFailure.c_str());
        }
      }
      if (key == m_activePreparationKey) {
        m_appliedSettings = settings;
        m_preparationFailure.clear();
      }
    }
    m_traceOptions = normalized(m_appliedSettings.trace);
    m_traceOptions.sharcReset = m_sharcReset;
    preparePostProcessing();
    std::copy(m_appliedSettings.clearColor.begin(),
              m_appliedSettings.clearColor.end(), m_clearColor);
    m_loading = (m_preparation && needsNgx) ||
                (settings.backend && m_traceSupported && !m_pathTracer && !holding) ||
                m_ngxPreparing->load() != 0;
  }

  upscale::Statistics RemixNativeRuntime::reconstructionStatistics() const {
    const auto *busy = busyState();
    auto result = busy ? busy->upscaling : m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
    result.requested = m_profiles.current().trace.upscaler;
    if (m_ngx) {
      const auto caps = m_ngx->capabilities();
      result.srSupported = caps.sr; result.rrSupported = caps.rr; result.sdkAvailable = caps.sdk;
      if (result.failure.empty() && !result.active) result.failure = caps.reason;
    }
    return result;
  }
  config::SkyState RemixNativeRuntime::skyState() const {
    return m_pathTracer ? m_pathTracer->skyState() : config::SkyState{};
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
    const auto *busy = busyState();
    auto result = busy           ? busy->tracing
                  : m_pathTracer ? m_pathTracer->statistics()
                                 : remixapi_TracingStatisticsEXT{.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    result.redrawRequested =
        preparationPending() || (m_backend && m_postProcessing && postProcessingStatistics().adapting) ||
        (m_backend &&
         (result.redrawRequested || m_profiles.current().trace.accumulate)) ||
        m_backend != m_targetBackend ||
        m_profiles.current() != m_appliedSettings;
    return result;
  }
  remixapi_SharcStatisticsEXT RemixNativeRuntime::sharcStatistics() const {
    const auto *busy = busyState();
    auto result = busy           ? busy->sharc
                  : m_pathTracer ? m_pathTracer->sharcStatistics()
                                 : remixapi_SharcStatisticsEXT{.structSize = sizeof(remixapi_SharcStatisticsEXT)};
    result.requested = m_profiles.current().trace.sharc;
    if (!m_pathTracer)
      std::strncpy(result.unavailableReason, REMIX_HAS_SHARC ? "Path tracing is inactive" : "SHARC was disabled at build time",
                   sizeof(result.unavailableReason) - 1);
    return result;
  }
  remixapi_LocalLightGuidingStatisticsEXT RemixNativeRuntime::localLightGuidingStatistics() const {
    const auto *busy = busyState();
    auto result = busy ? busy->guiding : m_pathTracer ? m_pathTracer->localLightGuidingStatistics()
        : remixapi_LocalLightGuidingStatisticsEXT{.structSize = sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
    result.requested = m_profiles.current().trace.localGuiding;
    result.influenceRange = m_profiles.current().trace.localRange;
    return result;
  }
  remixapi_LocalLightSetStatisticsEXT RemixNativeRuntime::localLightSetStatistics() const {
    const auto *busy = busyState();
    return busy ? busy->localLightSet : m_pathTracer ? m_pathTracer->localLightSetStatistics()
        : remixapi_LocalLightSetStatisticsEXT{.structSize = sizeof(remixapi_LocalLightSetStatisticsEXT)};
  }
  remixapi_CameraCullingStatisticsEXT RemixNativeRuntime::cameraCullingStatistics() const {
    const auto *busy = busyState();
    return busy ? busy->cameraCulling : m_pathTracer ? m_pathTracer->cameraCullingStatistics()
        : remixapi_CameraCullingStatisticsEXT{.structSize = sizeof(remixapi_CameraCullingStatisticsEXT)};
  }
  remixapi_LocalLightStatisticsEXT RemixNativeRuntime::localLightStatistics() const {
    const auto *busy = busyState();
    auto result = busy ? busy->localLight : m_pathTracer ? m_pathTracer->localLightStatistics()
        : remixapi_LocalLightStatisticsEXT{.structSize = sizeof(remixapi_LocalLightStatisticsEXT)};
    result.enabled = m_profiles.current().trace.localLights;
    result.samples = m_profiles.current().trace.localSamples;
    return result;
  }
  remixapi_ShadowStatisticsEXT RemixNativeRuntime::shadowStatistics() const {
    const auto *busy = busyState();
    auto result = busy           ? busy->shadow
                  : m_pathTracer ? m_pathTracer->shadowStatistics()
                                 : remixapi_ShadowStatisticsEXT{.structSize = sizeof(remixapi_ShadowStatisticsEXT)};
    result.requested = m_profiles.current().trace.sigma;
    return result;
  }
  remixapi_DenoisingStatisticsEXT RemixNativeRuntime::denoisingStatistics() const {
    const auto *busy = busyState();
    auto result =
        busy           ? busy->denoising
        : m_pathTracer ? m_pathTracer->denoisingStatistics()
                       : remixapi_DenoisingStatisticsEXT{.structSize = sizeof(remixapi_DenoisingStatisticsEXT)};
    result.requestedDenoiser = m_profiles.current().trace.denoiser;
    result.requestedTaa = m_profiles.current().trace.taa;
    if (!m_pathTracer && ((m_profiles.current().trace.denoiser && !m_denoiseCapabilities.reblurShSupported) ||
                          (m_profiles.current().trace.taa && !m_denoiseCapabilities.taaSupported)))
      std::strncpy(result.unavailableReason, m_denoiseCapabilities.unavailableReason,
                   sizeof(result.unavailableReason) - 1);
    return result;
  }
  remixapi_ErrorCode RemixNativeRuntime::vramStatistics(remixapi_VramStats &stats) const {
    stats = {};
    if (!m_initialized)
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    if (!m_profiles.current().vramStatistics)
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    // OpenRemix has no central allocator: report the resources that own
    // device memory directly. Pipeline, descriptor, swapchain and upscaler
    // allocations are not itemized; the driver figures below include them.
    // While the render thread works on a frame: the last completed frame's.
    const auto *busy = busyState();
    const auto raster = busy ? busy->memory : renderer().memoryStatistics();
    stats.usedMaterialTextureBytes = raster.textureBytes;
    stats.forkTextureCacheCount = raster.textureCount;
    stats.usedBufferBytes = raster.geometryBytes;
    stats.poolRetainedBytes = raster.pooledBytes;
    if (m_pathTracer) {
      stats.usedAccelerationStructureBytes =
          busy ? busy->tracing.accelerationBytes : m_pathTracer->statistics().accelerationBytes;
      stats.usedRenderTargetBytes = busy ? busy->denoising.memoryBytes : m_pathTracer->denoisingStatistics().memoryBytes;
    }
    stats.totalUsedBytes = stats.usedMaterialTextureBytes + stats.usedBufferBytes +
                           stats.usedAccelerationStructureBytes + stats.usedRenderTargetBytes;
    stats.totalAllocatedBytes = stats.totalUsedBytes + stats.poolRetainedBytes;
    if (m_memoryBudget) {
      // Process-wide usage and budget of device-local heaps.
      VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
      VkPhysicalDeviceMemoryProperties2 memory = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                                  .pNext = &budget};
      vkGetPhysicalDeviceMemoryProperties2(m_physicalDevice, &memory);
      for (uint32_t i = 0; i < memory.memoryProperties.memoryHeapCount; ++i)
        if (memory.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
          stats.driverAllocatedBytes += budget.heapUsage[i];
          stats.driverBudgetBytes += budget.heapBudget[i];
        }
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void RemixNativeRuntime::publishState() {
    // Render thread idle: the last completed frame's values, for the getters
    // while the frame about to be handed over renders.
    auto &s = m_published;
    s.performance = m_framePerformance;
    s.breakdown = m_frameBreakdown;
    s.presentations = m_presentationCount;
    s.frameStarted = m_frameStarted;
    s.digest = (m_pathTracer ? m_pathTracer->frameDigest() : 0) * 31 + renderer().profile().digest;
    s.upscaling = m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
    s.post = m_postProcessing ? m_postProcessing->statistics() : postfx::Statistics{};
    if (m_pathTracer) {
      s.tracing = m_pathTracer->statistics();
      s.sharc = m_pathTracer->sharcStatistics();
      s.shadow = m_pathTracer->shadowStatistics();
      s.localLight = m_pathTracer->localLightStatistics();
      s.localLightSet = m_pathTracer->localLightSetStatistics();
      s.cameraCulling = m_pathTracer->cameraCullingStatistics();
      s.guiding = m_pathTracer->localLightGuidingStatistics();
      s.denoising = m_pathTracer->denoisingStatistics();
    } else {
      s.tracing = {.structSize = sizeof(remixapi_TracingStatisticsEXT)};
      s.sharc = {.structSize = sizeof(remixapi_SharcStatisticsEXT)};
      s.shadow = {.structSize = sizeof(remixapi_ShadowStatisticsEXT)};
      s.localLight = {.structSize = sizeof(remixapi_LocalLightStatisticsEXT)};
      s.localLightSet = {.structSize = sizeof(remixapi_LocalLightSetStatisticsEXT)};
      s.cameraCulling = {.structSize = sizeof(remixapi_CameraCullingStatisticsEXT)};
      s.guiding = {.structSize = sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
      s.denoising = {.structSize = sizeof(remixapi_DenoisingStatisticsEXT)};
    }
    // Taken by the render thread after its frame (a walk of every cached
    // texture and mesh), while GetVramStats answers.
    s.memory = m_profiles.current().vramStatistics ? m_renderedMemory : RasterRenderer::MemoryStatistics{};
    s.gpuTimings = m_gpuTimers.latest();
  }

  remixapi_FeatureAvailability RemixNativeRuntime::getFeatureAvailability() const {
    remixapi_FeatureAvailability result{};
#ifdef REMIX_HAS_NRD
    result.compiledFeatures = REMIXAPI_FEATURE_NRD_BIT;
    if (m_denoiseCapabilities.reblurShSupported)
      result.availableFeatures = REMIXAPI_FEATURE_NRD_BIT;
#endif
    return result;
  }
} // namespace remix
