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

#include "platform/timing.h"
#include "runtime/frame_snapshot.h"
#include "runtime/runtime.h"

#include <SDL3/SDL_timer.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

namespace remix {
void RemixNativeRuntime::presented() {
  const auto now = std::chrono::steady_clock::now();
  if (m_lastPresentation != std::chrono::steady_clock::time_point{})
    m_longestPresentationGap = std::max(
        m_longestPresentationGap,
        std::chrono::duration<double, std::milli>(now - m_lastPresentation)
            .count());
  m_lastPresentation = now;
  ++m_presentationCount;
}
  namespace {
    // A prepared frame that ends up not traced (presentation skipped, or an
    // error on the way) gives its lighting state back to the tracer that
    // prepared it, if that tracer is still the runtime's.
    struct LightingReturn {
      const std::unique_ptr<PathTracer> &tracer;
      const PathTracer *owner = tracer.get();
      scene::TraceFrame *frame = nullptr;
      ~LightingReturn() {
        if (frame && tracer && tracer.get() == owner)
          tracer->returnScene(*frame);
      }
    };
    struct CompletePerformance {
      remixapi_FramePerformanceEXT &out;
      remixapi_FrameBreakdownEXT &breakdown;
      PathTracer::StaticStatistics &statics;
      const std::unique_ptr<PathTracer> &tracer;
      const RasterRenderer &raster;
      const uint64_t &presentations;
      ~CompletePerformance() {
        out.presented = presentations != out.presentations;
        out.presentations = presentations;
        // Runtime-owned phases were accumulated in breakdown during recording.
        auto frame = breakdown;
        if (tracer) {
          frame = tracer->breakdown();
          frame.traceBeginMilliseconds = breakdown.traceBeginMilliseconds;
          frame.compositeMilliseconds = breakdown.compositeMilliseconds;
          frame.uiMilliseconds = breakdown.uiMilliseconds;
          frame.snapshotMilliseconds = breakdown.snapshotMilliseconds;
          frame.presentSetupMilliseconds = breakdown.presentSetupMilliseconds;
          frame.presentReleaseMilliseconds = breakdown.presentReleaseMilliseconds;
        }
        const auto &r = raster.profile();
        frame.rasterPrepareMilliseconds = breakdown.rasterPrepareMilliseconds ? r.prepareMilliseconds : 0;
        frame.rasterSkinningMilliseconds = breakdown.rasterPrepareMilliseconds ? r.skinningMilliseconds : 0;
        frame.rasterDraws = r.draws;
        frame.rasterSkinnedVertices = r.skinnedVertices;
        frame.rasterUploadBytes = r.uploadBytes;
        frame.rasterCulledDraws = r.culledDraws;
        const double covered =
            frame.rasterPrepareMilliseconds + frame.traceBeginMilliseconds + frame.traceSetupMilliseconds +
            frame.traceSurfacesMilliseconds + frame.retireMilliseconds + frame.traceAccelerationMilliseconds +
            frame.tableMilliseconds + frame.lightingMilliseconds + frame.sharcMilliseconds +
            frame.signatureMilliseconds + frame.descriptorMilliseconds + frame.primaryMilliseconds +
            frame.passesMilliseconds + frame.postMilliseconds + frame.compositeMilliseconds +
            frame.uiMilliseconds + frame.snapshotMilliseconds;
        frame.otherMilliseconds = std::max(0.0, out.recordMilliseconds - covered);
        breakdown = frame;
        statics = tracer ? tracer->staticStatistics() : PathTracer::StaticStatistics{};
        if (!tracer)
          return;
        const auto p = tracer->performance();
        out.geometryMilliseconds = p.geometryMilliseconds;
        out.allocationMilliseconds = p.allocationMilliseconds;
        out.uploadMilliseconds = p.uploadMilliseconds;
        out.accelerationMilliseconds = p.accelerationMilliseconds;
        out.geometryConversions = p.geometryConversions;
        out.geometryCacheHits = p.geometryCacheHits;
        out.uploadBytes = p.uploadBytes;
        out.bufferAllocations = p.bufferAllocations;
        out.allocationBytes = p.allocationBytes;
        out.bufferCacheHits = p.bufferCacheHits;
        out.memoryAllocations = p.memoryAllocations;
        out.memoryAllocationBytes = p.memoryAllocationBytes;
      }
    };
  } // namespace
  void RemixNativeRuntime::logFrameGeneration(const char *format, ...) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (!m_frameGenerationLog) {
      std::filesystem::path path = "openremix_framegen.log";
#if defined(_WIN32)
      // Beside this library (the host's working directory may be anywhere).
      static const int anchor = 0;
      HMODULE module{};
      std::wstring file(32768, L'\0');
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&anchor), &module)) {
        const auto size = GetModuleFileNameW(module, file.data(), DWORD(file.size()));
        if (size && size < file.size()) {
          file.resize(size);
          path = std::filesystem::path(file).parent_path() / "openremix_framegen.log";
        }
      }
      m_frameGenerationLog = _wfopen(path.c_str(), L"w");
#else
      m_frameGenerationLog = std::fopen(path.c_str(), "w");
#endif
      if (!m_frameGenerationLog)
        return;
      m_frameGenerationLogStart = std::chrono::steady_clock::now();
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_frameGenerationLogStart).count();
    std::fprintf(m_frameGenerationLog, "[%9.3f] ", seconds);
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(m_frameGenerationLog, format, arguments);
    va_end(arguments);
    std::fputc('\n', m_frameGenerationLog);
    std::fflush(m_frameGenerationLog);
  }

  void RemixNativeRuntime::logFrameGenerationState() {
    // Nothing is written until frame generation has been asked for once.
    if (!m_frameGenerationRequested && !m_frameGenerationLog)
      return;
    if (!m_frameGenerationLogHeader) {
      m_frameGenerationLogHeader = true;
      logFrameGeneration("build: " __DATE__ " " __TIME__ " (frame pacing after the GPU, 3)");
      logFrameGeneration("device: %s | vendor 0x%04x device 0x%04x | driver version %u (raw) | Vulkan %u.%u.%u",
                         m_properties.deviceName, m_properties.vendorID, m_properties.deviceID,
                         m_properties.driverVersion, VK_API_VERSION_MAJOR(m_properties.apiVersion),
                         VK_API_VERSION_MINOR(m_properties.apiVersion), VK_API_VERSION_PATCH(m_properties.apiVersion));
    }
    const auto &settings = m_profiles.current();
    std::string status = m_frameGenerationStatus.empty() ? "(none yet)" : m_frameGenerationStatus;
    status += " | setting " + std::to_string(int(settings.frameGeneration)) + ", frames " +
              std::to_string(settings.frameGenerationFrames) + ", asked " + std::to_string(m_frameGenerationRequested) +
              ", swapchain made for " + std::to_string(m_presenterFrameGeneration) + ", generating " +
              std::to_string(m_generateCount) + ", ready " + std::to_string(int(m_frameGenerationReady)) +
              ", failed " + std::to_string(int(m_frameGenerationFailed)) + ", async present " +
              std::to_string(int(m_asyncActive)) + ", low latency " + std::to_string(int(m_lowLatencyActive));
    if (status != m_loggedFrameGenerationStatus) {
      m_loggedFrameGenerationStatus = status;
      std::string detail;
      if (m_ngx) {
        const auto ngx = m_ngx->capabilities();
        detail += " | NGX: sdk " + std::to_string(int(ngx.sdk)) + ", initialized " +
                  std::to_string(int(ngx.initializationComplete)) + ", frame generation " + std::to_string(int(ngx.fg)) +
                  " (up to " + std::to_string(ngx.fgMultiFrameMax) + " generated)" +
                  (ngx.fgReason.empty() ? "" : ", reason: " + ngx.fgReason);
      }
      if (m_pathTracer) {
        const auto upscaling = m_pathTracer->upscalingStatistics();
        detail += std::string(" | upscaler ") +
                  (upscaling.provider.empty() ? upscale::name(upscaling.backend) : upscaling.provider) + " " +
                  std::to_string(upscaling.render.width) + "x" + std::to_string(upscaling.render.height) + " -> " +
                  std::to_string(upscaling.display.width) + "x" + std::to_string(upscaling.display.height);
      } else {
        detail += " | no path tracer";
      }
      logFrameGeneration("status: %s%s", status.c_str(), detail.c_str());
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_frameGenerationLogged != std::chrono::steady_clock::time_point{} &&
        now - m_frameGenerationLogged < std::chrono::seconds(5))
      return;
    const double seconds = m_frameGenerationLogged == std::chrono::steady_clock::time_point{}
                               ? 0
                               : std::chrono::duration<double>(now - m_frameGenerationLogged).count();
    m_frameGenerationLogged = now;
    // Counters are read while the render thread may be writing them: a line
    // can be a frame off, which does not matter here.
    const uint64_t generated = m_generatedFrames, duplicated = m_duplicatedFrames, short_ = m_ungeneratedFrames,
                   presentations = m_presentationCount;
    const uint32_t generatedShownCount = m_generatedShownCount.exchange(0),
                   renderedShownCount = m_renderedShownCount.exchange(0);
    const double generatedShown =
        double(m_generatedShownMicroseconds.exchange(0)) / 1000.0 / std::max(generatedShownCount, 1u);
    const double renderedShown =
        double(m_renderedShownMicroseconds.exchange(0)) / 1000.0 / std::max(renderedShownCount, 1u);
    if (seconds > 0 && generated != m_loggedGenerated)
      logFrameGeneration("  timing, min/average/max ~change from one to the next, ms: generated frame on screen %s | rendered frame on screen %s | "
                         "host's rendered frames apart %s | GPU, submission to completion (waits for the frame "
                         "before it included) %s | host waiting for the previous frame's images %s",
                         m_logGenerated.take().c_str(), m_logRendered.take().c_str(), m_logHost.take().c_str(),
                         m_logGpu.take().c_str(), m_logWait.take().c_str());
    if (seconds > 0)
      logFrameGeneration("last %.1f s: rendered frames presented %llu (%.1f per second) | generated %llu (%.1f per "
                         "second), shown twice instead %llu, short of generated ones %llu | rendered interval %.2f ms | "
                         "latest present late by at most %.2f ms | on screen for, on average: generated frames %.2f "
                         "ms, rendered frames %.2f ms | last failed acquire result %d | totals: generated "
                         "%llu, shown twice %llu, short %llu | paced by the driver (present metering) %u frames, "
                         "refused %u",
                         seconds, (unsigned long long)(presentations - m_loggedPresentations),
                         double(presentations - m_loggedPresentations) / seconds,
                         (unsigned long long)(generated - m_loggedGenerated),
                         double(generated - m_loggedGenerated) / seconds,
                         (unsigned long long)(duplicated - m_loggedDuplicated),
                         (unsigned long long)(short_ - m_loggedUngenerated), m_renderedInterval,
                         double(m_presentLateMicroseconds.exchange(0)) / 1000.0, generatedShown, renderedShown,
                         m_lastGeneratedAcquire,
                         (unsigned long long)generated, (unsigned long long)duplicated, (unsigned long long)short_,
                         m_meteredFrames.exchange(0), m_meteringRefused.exchange(0));
    m_loggedGenerated = generated;
    m_loggedDuplicated = duplicated;
    m_loggedUngenerated = short_;
    m_loggedPresentations = presentations;
  }

  bool RemixNativeRuntime::latencyWanted() const {
    // The driver's low latency mode is for a host that would otherwise queue
    // its frames behind the GPU: the path tracer's, or frame generation, whose
    // presents are paced on it. With the raster backend the GPU is idle for
    // most of a frame and the mode only took time, on (the sleep after
    // Present) or merely enabled (the driver then holds vkQueuePresentKHR).
    // Garry's Mod, gm_voidplaces, raster: 19.6 ms a frame with the sleep,
    // 17.4 with the mode enabled and no sleep, 13.6 with it off.
    return m_lowLatencyRequested && (m_frameGenerationRequested || m_backend == 1);
  }

  void RemixNativeRuntime::applyLatencyMode() {
    if (!m_lowLatencySupported || !m_swapchain)
      return;
    const VkLatencySleepModeInfoNV mode = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
                                           .lowLatencyMode = latencyWanted() ? VK_TRUE : VK_FALSE,
                                           .lowLatencyBoost = VK_FALSE,
                                           .minimumIntervalUs = 0};
    std::lock_guard lock(m_queueMutex);
    if (m_setLatencySleepMode(m_device, m_swapchain, &mode) == VK_SUCCESS)
      m_lowLatencyActive = latencyWanted();
  }

  void RemixNativeRuntime::latencyMarker(VkLatencyMarkerNV marker, uint64_t frame) {
    if (!m_lowLatencyActive || !m_swapchain)
      return;
    // The driver serializes a marker behind a sleep in progress: while one
    // overruns (the host has stopped waiting for it), a marker would hold
    // this thread for the rest of it, the host's or the render thread's.
    // Those frames go unmarked.
    if (m_latencySleeper.sleeping.load(std::memory_order_relaxed))
      return;
    const VkSetLatencyMarkerInfoNV info = {.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
                                           .presentID = frame,
                                           .marker = marker};
    m_setLatencyMarker(m_device, m_swapchain, &info);
  }

  void RemixNativeRuntime::LatencySleeper::run(std::function<void()> work, double milliseconds) {
    std::unique_lock lock(mutex);
    if (!busy) {
      if (!thread.joinable())
        thread = std::thread([this] {
          std::unique_lock lock(mutex);
          for (;;) {
            wake.wait(lock, [&] { return stop || job; });
            if (stop)
              return;
            auto current = std::move(job);
            job = nullptr;
            sleeping.store(true, std::memory_order_relaxed);
            lock.unlock();
            current();
            lock.lock();
            sleeping.store(false, std::memory_order_relaxed);
            busy = false;
            done.notify_all();
          }
        });
      job = std::move(work);
      busy = true;
      wake.notify_one();
    }
    done.wait_for(lock, std::chrono::duration<double, std::milli>(milliseconds), [&] { return !busy; });
  }

  void RemixNativeRuntime::LatencySleeper::idle() {
    std::unique_lock lock(mutex);
    done.wait(lock, [&] { return !busy; });
  }

  void RemixNativeRuntime::LatencySleeper::shutdown() {
    {
      std::unique_lock lock(mutex);
      done.wait(lock, [&] { return !busy; });
      stop = true;
      wake.notify_one();
    }
    if (thread.joinable())
      thread.join();
    stop = false;
  }

  void RemixNativeRuntime::latencySleep() {
    m_latencySleepMilliseconds = 0;
    if (!m_lowLatencyActive || !m_swapchain)
      return;
    ScopedMilliseconds sleepTime{m_latencySleepMilliseconds};
    // The driver now and then holds the host for several frames' time after
    // a long frame (80-90 ms seen, at 21 ms a frame): a second long frame for
    // every first. A sleep is worth at most about a frame, so that is the
    // most it gets; the driver's signal, when it comes, is for a value no
    // wait asks for any more.
    const auto now = std::chrono::steady_clock::now();
    if (m_latencySleptAt != std::chrono::steady_clock::time_point{}) {
      const double interval = std::chrono::duration<double, std::milli>(now - m_latencySleptAt).count();
      // Long frames do not lengthen the bound much: they are what it is for.
      const double sample = std::min(interval, 2 * std::max(m_latencyIntervalMilliseconds, 1.));
      m_latencyIntervalMilliseconds =
          m_latencyIntervalMilliseconds > 0 ? m_latencyIntervalMilliseconds * .9 + sample * .1 : interval;
    }
    m_latencySleptAt = now;
    const double bound = std::clamp(m_latencyIntervalMilliseconds > 0 ? m_latencyIntervalMilliseconds : 100., 8., 100.);
    const uint64_t value = ++m_latencyValue;
    m_latencySleeper.run(
        [this, value] {
          const VkLatencySleepInfoNV sleep = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
                                              .signalSemaphore = m_latencySemaphore,
                                              .value = value};
          if (m_latencySleep(m_device, m_swapchain, &sleep) == VK_SUCCESS) {
            // Bounded: a driver that never signals must not hold the thread.
            const VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                              .semaphoreCount = 1,
                                              .pSemaphores = &m_latencySemaphore,
                                              .pValues = &value};
            vkWaitSemaphores(m_device, &wait, 100000000);
          }
        },
        bound);
    // The host's next frame begins here.
    ++m_latencyFrame;
    latencyMarker(VK_LATENCY_MARKER_SIMULATION_START_NV, m_latencyFrame);
  }

  // Present, then the driver's pacing of the host (NVIDIA Reflex).
  remixapi_ErrorCode RemixNativeRuntime::present(const remixapi_PresentInfo *info) {
    latencyMarker(VK_LATENCY_MARKER_SIMULATION_END_NV, m_latencyFrame);
    const auto status = presentUnpaced(info);
    logFrameGenerationState();
    latencySleep();
    return status;
  }

  remixapi_ErrorCode RemixNativeRuntime::presentUnpaced(const remixapi_PresentInfo *info) {
    if (info && info->sType != REMIXAPI_STRUCT_TYPE_PRESENT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!isInitialized()) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    // "rtx.native.asyncPresent". The headless editor acknowledges each frame
    // only once it is complete, so it always presents synchronously.
    const bool async = m_profiles.current().asyncPresent && !m_headless;
    if (!async && !m_asyncActive) {
      if (latencyWanted() != m_lowLatencyActive)
        applyLatencyMode();
      m_latencyRenderFrame = m_latencyFrame;
      return presentFrame(info);
    }
    // At most one frame in flight on the CPU side: the render thread finishes
    // the previous one before this Present touches anything it owns.
    const auto start = std::chrono::steady_clock::now();
    const double waited = waitRenderThread();
    m_waitedFrame = {m_asyncRender.renderMilliseconds,       m_framePerformance.presentMilliseconds,
                     m_framePerformance.recordMilliseconds,  m_framePerformance.fenceMilliseconds,
                     m_framePerformance.acquireMilliseconds, m_framePerformance.submitMilliseconds,
                     m_framePerformance.queuePresentMilliseconds,
                     m_frameBreakdown.presentReleaseMilliseconds,
                     m_framePerformance.sceneMilliseconds,
                     m_frameBreakdown.presentSetupMilliseconds,
                     m_queueLockMilliseconds,
                     m_frameBreakdown.rasterPrepareMilliseconds,
                     m_frameBreakdown.traceSurfacesMilliseconds,
                     m_frameBreakdown.materialMilliseconds,
                     m_frameBreakdown.traceAccelerationMilliseconds,
                     m_frameBreakdown.tableMilliseconds,
                     m_frameBreakdown.passesMilliseconds,
                     m_frameBreakdown.compositeMilliseconds,
                     m_framePerformance.geometryMilliseconds,
                     m_framePerformance.uploadMilliseconds,
                     m_framePerformance.allocationMilliseconds,
                     m_framePerformance.bufferAllocations,
                     m_framePerformance.memoryAllocations};
    if (latencyWanted() != m_lowLatencyActive)
      applyLatencyMode();
    m_latencyRenderFrame = m_latencyFrame;
    if (async != m_asyncActive) {
      try {
        switchPresentation(async);
      } catch (...) {
        return fail();
      }
    }
    if (!async)
      return presentFrame(info);
    return presentAsync(info, start, waited);
  }

  double RemixNativeRuntime::waitRenderThread() {
    if (!m_renderThread.started())
      return 0;
    const double waited = m_renderThread.wait();
    // renderFrame handles its own failures; anything escaping is a failure too.
    if (m_renderThread.takeError())
      fail();
    return waited;
  }

  void RemixNativeRuntime::switchPresentation(bool async) {
    // The render thread is idle. The GPU state (caches, pipelines, target)
    // follows the frame being rendered; the static set starts a new epoch, so
    // the tracer's copy is rebuilt from whichever table it now syncs from.
    if (async) {
      if (!m_frameScene)
        m_frameScene = std::make_unique<RasterRenderer>();
      m_frameScene->reset();
      m_frameScene->adoptGpu(m_raster);
    } else {
      m_raster.adoptGpu(*m_frameScene);
      m_frameScene->reset(); // Releases the last published frame and the replica.
    }
    m_raster.restartStatic();
    m_raster.setConcurrentConsumer(async);
    m_asyncActive = async;
    m_asyncCaller.active = async;
    SDL_Log("OpenRemix presentation: %s", async ? "asynchronous (render thread)" : "synchronous");
  }

  remixapi_ErrorCode RemixNativeRuntime::presentAsync(const remixapi_PresentInfo *info,
                                                      std::chrono::steady_clock::time_point start,
                                                      double waited) {
    // The render thread's last frame is complete: Diagnostics shows it, and
    // the getters return it while the next one renders.
    m_previousPerformance = m_framePerformance;
    m_previousBreakdown = m_frameBreakdown;
    m_previousStatic = m_staticStatistics;
    m_previousAsyncCaller = m_asyncCaller;
    m_asyncCaller = {.active = true, .waitMilliseconds = waited, .handedOff = m_asyncCaller.handedOff};
    struct CallTime {
      double &total;
      std::chrono::steady_clock::time_point start;
      ~CallTime() {
        total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      }
    } callTime{m_asyncCaller.callMilliseconds, start};
    remixapi_WindowState state = {};
    auto status = m_window.getWindowState(state);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
      return status;
    }
#if defined(_WIN32)
    if (info && info->hwndOverride &&
        info->hwndOverride != SDL_GetPointerProperty(SDL_GetWindowProperties(m_window.handle()),
                                                     SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
#endif
    if (m_failed) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    auto &frame = *m_frameScene;
    // A frame that is not handed over ends here, as a synchronous Present
    // would end it: the API's draw lists, and the published copy if any.
    struct Unsent {
      NativeResources &api, &frame;
      bool published = false, handedOff = false;
      ~Unsent() {
        if (handedOff)
          return;
        api.finishFrame();
        if (published)
          frame.finishFrame();
      }
    } unsent{m_raster, frame};
    Stopwatch setupTime{m_asyncCaller.setupMilliseconds};
    if (!m_window.isVulkanSurfaceCurrent()) {
      m_recreateSurface = true;
    }
    applySettings();
    if (state.minimized || !state.drawableWidth || !state.drawableHeight) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    try {
      // Held from the swapchain check to the handoff: the setup may submit
      // (NGX feature creation) or wait for the device (a tracer rebuilt for
      // new settings) while the presenter shows the previous frame.
      std::unique_lock queueLock(m_queueMutex, std::defer_lock);
      if (m_recreateSurface || m_rebuild || !m_swapchain || m_vsync != m_presenterVsync ||
          m_frameGenerationRequested != m_presenterFrameGeneration || wantsHdr10() != m_hdr10Requested ||
          state.drawableWidth != m_swapchainExtent.width || state.drawableHeight != m_swapchainExtent.height) {
        status = recreateSwapchain();
        if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
          return fail();
        }
      }
      if (!m_swapchain) {
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      queueLock.lock();
      updateFreeCamera();
      m_raster.applyFreeCamera(m_freeCamera);
      // The frame snapshot: from here on API calls change the next frame.
      setupTime.stop();
      measured(m_asyncCaller.captureMilliseconds, [&] { m_raster.publishFrame(frame); });
      unsent.published = true;
      publishParticles();
      setupTime.start();
      // As the synchronous path, reading the published frame.
      prepareSettings();
      {
        const auto &settings = m_profiles.current();
        const bool traceStatic = m_pathTracer && m_targetBackend && m_backend != 0 && !m_loading;
        if (!traceStatic)
          frame.expandStatic();
        // From the next replay on; the tracer reads the setting this frame.
        m_raster.setPersistentStatic(settings.persistentStatic && traceStatic);
        m_raster.publishStaticSetting(frame);
        if (m_pathTracer)
          m_pathTracer->setStaticValidation(settings.persistentStatic && settings.persistentStaticValidation);
      }
      if (m_pathTracer)
        m_pathTracer->setSky(m_profiles.current().sky, m_skyControl, m_skyUnavailable);
      frame.setSkyTarget(m_pathTracer ? m_pathTracer->skyPass() : VK_NULL_HANDLE);
      m_rasterPreparing = !frame.preparePipelines(m_backend != 0);
      m_loading |= m_rasterPreparing;
      if (m_loading && m_postProcessing) m_postProcessing->freeze();
      prepareFrameGeneration();
      // The developer UI reads and edits settings and statistics, and SDL
      // input capture belongs to this thread: build its frame here. The
      // render thread only records the finished draw data (and ImGui's
      // current context is per thread).
      measured(m_asyncCaller.uiMilliseconds, [&] { drawUi(VK_NULL_HANDLE); });
      publishState();
      setupTime.stop();
      queueLock.unlock();
      m_handoff = {.callerMilliseconds = m_asyncCaller.setupMilliseconds + m_asyncCaller.captureMilliseconds,
                   .memoryStatistics = m_profiles.current().vramStatistics};
      m_renderThread.submit([this] { renderFrame(); });
      // From here the render thread owns the published frame.
      unsent.handedOff = true;
      ++m_asyncCaller.handedOff;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    } catch (VkResult result) {
      return fail(result);
    } catch (...) {
      return fail();
    }
  }

  // The render thread's part of an asynchronous Present: what the synchronous
  // path does after its setup, from the published frame. Touches only
  // render-owned state (runtime.h); failures are recorded (m_failed) for the
  // next Present, since this frame's Present has already returned.
  void RemixNativeRuntime::renderFrame() {
    m_asyncRender.renderMilliseconds = 0;
    m_queueLockMilliseconds = 0;
    ScopedMilliseconds renderTime{m_asyncRender.renderMilliseconds};
    auto &frame = *m_frameScene;
    // GetVramStats' raster figures walk every cached texture and mesh: taken
    // here, off the caller's thread, for the next handoff to publish.
    struct Memory {
      const RasterRenderer &frame;
      RasterRenderer::MemoryStatistics &out;
      bool enabled;
      ~Memory() {
        if (enabled)
          out = frame.memoryStatistics();
      }
    } memory{frame, m_renderedMemory, m_handoff.memoryStatistics};
    auto &p = m_framePerformance;
    p = {.structSize = sizeof(p), .attempts = p.attempts + 1, .presentations = m_presentationCount};
    m_frameBreakdown = {.structSize = sizeof(m_frameBreakdown)};
    // The caller's setup and capture are this frame's Present setup.
    p.presentMilliseconds = m_handoff.callerMilliseconds;
    m_frameBreakdown.presentSetupMilliseconds = m_handoff.callerMilliseconds;
    // Destroyed after presentTime, so recordMilliseconds is final.
    CompletePerformance completePerformance{p,        m_frameBreakdown, m_staticStatistics,
                                            m_pathTracer, frame,          m_presentationCount};
    ScopedMilliseconds presentTime{p.presentMilliseconds};
    if (m_pathTracer)
      m_pathTracer->resetPerformance();
    struct EndFrame {
      RasterRenderer &raster;
      double &release;
      ~EndFrame() { measured(release, [&] { raster.finishFrame(); }); }
    } endFrame{frame, m_frameBreakdown.presentReleaseMilliseconds};
    Stopwatch setupTime{m_frameBreakdown.presentSetupMilliseconds};
    try {
      // Held while the frame is recorded and submitted (see presentImages).
      std::unique_lock queueLock(m_queueMutex, std::defer_lock);
      using Clock = std::chrono::steady_clock;
      struct ReleaseEnd {
        Clock::time_point &start;
        double &release;
        ~ReleaseEnd() {
          if (start != Clock::time_point{})
            release += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
      };
      Clock::time_point releaseStart{};
      ReleaseEnd releaseEnd{releaseStart, m_frameBreakdown.presentReleaseMilliseconds};
      std::optional<scene::TraceFrame> traceScene;
      struct ReleaseStart {
        Clock::time_point &start;
        ~ReleaseStart() { start = Clock::now(); }
      } releaseStartStamp{releaseStart};
      setupTime.stop();
      if (m_pathTracer && m_targetBackend && !m_loading)
        traceScene = measured(p.sceneMilliseconds, [&] { return m_pathTracer->prepareScene(frame, m_traceOptions); });
      LightingReturn lightingReturn{m_pathTracer};
      if (traceScene)
        lightingReturn.frame = &*traceScene;
      VkResult result = measured(p.fenceMilliseconds, [&] {
        return vkWaitForFences(m_device, 1, &m_frames[m_frameIndex].fence, VK_TRUE, 1000000000);
      });
      if (result == VK_TIMEOUT)
        return;
      if (result != VK_SUCCESS) {
        fail(result);
        return;
      }
      setupTime.start();
      m_frames[m_frameIndex].snapshot.reset();
      frame.selectFrame(m_frameIndex);
      if (m_particles) m_particles->selectFrame(m_frameIndex);
      if (m_postProcessing) m_postProcessing->selectFrame(m_frameIndex);
      if (m_pathTracer)
        m_pathTracer->selectFrame(m_frameIndex);
      uint32_t imageIndex = 0;
      m_screenshot.complete(m_device, true);
      setupTime.stop();
      result = measured(p.acquireMilliseconds, [&] {
        return acquireImage(m_frames[m_frameIndex].imageAvailable, 1000000000, imageIndex);
      });
      if (result == VK_TIMEOUT || result == VK_NOT_READY)
        return;
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
        // The next Present recreates the swapchain (render thread idle).
        m_rebuild = true;
        m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        return;
      }
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fail(result);
        return;
      }
      m_rebuild |= result == VK_SUBOPTIMAL_KHR;
      m_frameStarted = true;
      latencyMarker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, m_latencyRenderFrame);
      measured(m_queueLockMilliseconds, [&] { queueLock.lock(); });
      const GeneratedImages generated = acquireGenerated();
      const auto status = measured(p.recordMilliseconds, [&] {
        return recordFrame(imageIndex, traceScene ? &*traceScene : nullptr, generated);
      });
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        fail();
        return;
      }
      result = vkResetFences(m_device, 1, &m_frames[m_frameIndex].fence);
      if (result != VK_SUCCESS) {
        fail(result);
        return;
      }
      const VkPipelineStageFlags waitStage =
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      // With generated frames: every acquired image, and a semaphore for each present.
      VkSemaphore waits[1 + FrameGeneration::maxGenerated] = {m_frames[m_frameIndex].imageAvailable};
      VkPipelineStageFlags waitStages[1 + FrameGeneration::maxGenerated] = {waitStage};
      VkSemaphore signals[1 + FrameGeneration::maxGenerated] = {m_renderFinished[imageIndex]};
      for (uint32_t i = 0; i < generated.count; ++i) {
        waits[1 + i] = m_frames[m_frameIndex].generatedAvailable[i];
        waitStages[1 + i] = waitStage;
        signals[1 + i] = m_renderFinished[generated.index[i]];
      }
      const VkSubmitInfo submit = {
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
          .waitSemaphoreCount = 1 + generated.count,
          .pWaitSemaphores = waits,
          .pWaitDstStageMask = waitStages,
          .commandBufferCount = 1,
          .pCommandBuffers = &m_commandBuffers[m_frameIndex],
          .signalSemaphoreCount = 1 + generated.count,
          .pSignalSemaphores = signals,
      };
      result = measured(p.submitMilliseconds,
                        [&] { return vkQueueSubmit(m_queue, 1, &submit, m_frames[m_frameIndex].fence); });
      if (result != VK_SUCCESS) {
        fail(result);
        return;
      }
      latencyMarker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, m_latencyRenderFrame);
      queueLock.unlock();
      m_screenshot.submitted(m_frames[m_frameIndex].fence);
      if (m_pathTracer && !m_loading)
        measured(m_frameBreakdown.presentReleaseMilliseconds, [&] { m_pathTracer->submitted(); });
      result = measured(p.queuePresentMilliseconds, [&] { return presentImages(imageIndex, generated); });
      m_frameIndex = (m_frameIndex + 1) % m_frameCount;
      if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        presented();
        ++m_asyncRender.rendered;
      }
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
          result == VK_ERROR_SURFACE_LOST_KHR) {
        m_rebuild = true;
        m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        return;
      }
      if (result != VK_SUCCESS)
        fail(result);
    } catch (VkResult result) {
      fail(result);
    } catch (...) {
      fail();
    }
  }

  // Synchronous presentation (asynchronous presentation off, render thread
  // idle): m_raster is both the API's scene and the renderer.
  remixapi_ErrorCode RemixNativeRuntime::presentFrame(const remixapi_PresentInfo *info) {
    if (info && info->sType != REMIXAPI_STRUCT_TYPE_PRESENT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!isInitialized()) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    auto &p = m_framePerformance;
    m_previousPerformance = p;
    m_previousBreakdown = m_frameBreakdown;
    m_previousStatic = m_staticStatistics;
    p = {.structSize = sizeof(p), .attempts = p.attempts + 1, .presentations = m_presentationCount};
    m_frameBreakdown = {.structSize = sizeof(m_frameBreakdown)};
    // Destroyed after presentTime, so recordMilliseconds is final.
    CompletePerformance completePerformance{p,        m_frameBreakdown, m_staticStatistics,
                                            m_pathTracer, m_raster,       m_presentationCount};
    ScopedMilliseconds presentTime{p.presentMilliseconds};
    if (m_pathTracer)
      m_pathTracer->resetPerformance();
    remixapi_WindowState state = {};
    auto status = m_headless ? REMIXAPI_ERROR_CODE_SUCCESS : m_window.getWindowState(state);
    if (m_headless) {
      state.drawableWidth = m_swapchainExtent.width;
      state.drawableHeight = m_swapchainExtent.height;
    }
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
      return status;
    }
#if defined(_WIN32)
    if (info && info->hwndOverride &&
        info->hwndOverride != SDL_GetPointerProperty(SDL_GetWindowProperties(m_window.handle()),
                                                     SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
#endif
    if (m_failed) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    struct EndFrame {
      RasterRenderer &raster;
      double &release;
      ~EndFrame() { measured(release, [&] { raster.finishFrame(); }); }
    } endFrame{m_raster, m_frameBreakdown.presentReleaseMilliseconds};
    Stopwatch setupTime{m_frameBreakdown.presentSetupMilliseconds};
    if (!m_headless && !m_window.isVulkanSurfaceCurrent()) {
      m_recreateSurface = true;
    }
    applySettings();
    if (state.minimized || !state.drawableWidth || !state.drawableHeight) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    try {
      // Held during the setup and while the frame is recorded and submitted
      // (see presentImages); released for the waits in between.
      std::unique_lock queueLock(m_queueMutex, std::defer_lock);
      if (!m_headless && (m_recreateSurface || m_rebuild || !m_swapchain || m_vsync != m_presenterVsync ||
                          m_frameGenerationRequested != m_presenterFrameGeneration ||
                          wantsHdr10() != m_hdr10Requested || state.drawableWidth != m_swapchainExtent.width ||
                          state.drawableHeight != m_swapchainExtent.height)) {
        status = recreateSwapchain();
        if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
          return fail();
        }
      }
      if (!m_swapchain && !m_sharedImage) {
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      queueLock.lock();
      updateFreeCamera();
      m_raster.applyFreeCamera(m_freeCamera);
      publishParticles();
      prepareSettings();
      {
        // A persistent static set is traced from the tracer's own copy of it.
        // Any other consumer of this frame (raster rendering, a frame without
        // the tracer) takes the set into the draw list, as without it.
        const auto &settings = m_profiles.current();
        const bool traceStatic = m_pathTracer && m_targetBackend && m_backend != 0 && !m_loading;
        if (!traceStatic)
          m_raster.expandStatic();
        // From the next replay on.
        m_raster.setPersistentStatic(settings.persistentStatic && traceStatic);
        if (m_pathTracer)
          m_pathTracer->setStaticValidation(settings.persistentStatic && settings.persistentStaticValidation);
      }
      if (m_pathTracer)
        m_pathTracer->setSky(m_profiles.current().sky, m_skyControl, m_skyUnavailable);
      m_raster.setSkyTarget(m_pathTracer ? m_pathTracer->skyPass()
                                         : VK_NULL_HANDLE);
      m_rasterPreparing = !m_raster.preparePipelines(m_backend != 0);
      m_loading |= m_rasterPreparing;
      if (m_loading && m_postProcessing) m_postProcessing->freeze();
      prepareFrameGeneration();
      // Scene snapshots, emission reduction and staged light hierarchies use
      // no GPU resources. Prepare while submitted frames run; commit lighting,
      // upload and retire resources only after the reused slot's fence.
      // The trace frame's release is timed from ReleaseStart's destruction
      // (declared after it) to ReleaseEnd's (declared before it).
      using Clock = std::chrono::steady_clock;
      struct ReleaseEnd {
        Clock::time_point &start;
        double &release;
        ~ReleaseEnd() {
          if (start != Clock::time_point{})
            release += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
      };
      Clock::time_point releaseStart{};
      ReleaseEnd releaseEnd{releaseStart, m_frameBreakdown.presentReleaseMilliseconds};
      std::optional<scene::TraceFrame> traceScene;
      struct ReleaseStart {
        Clock::time_point &start;
        ~ReleaseStart() { start = Clock::now(); }
      } releaseStartStamp{releaseStart};
      setupTime.stop();
      queueLock.unlock();
      if (m_pathTracer && m_targetBackend && !m_loading)
        traceScene = measured(p.sceneMilliseconds,
                              [&] { return m_pathTracer->prepareScene(m_raster, m_traceOptions); });
      LightingReturn lightingReturn{m_pathTracer};
      if (traceScene)
        lightingReturn.frame = &*traceScene;
      // Bounded waits keep unavailable surfaces and a stalled GPU from blocking the caller indefinitely.
      VkResult result = measured(p.fenceMilliseconds, [&] {
        return vkWaitForFences(m_device, 1, &m_frames[m_frameIndex].fence, VK_TRUE, 1000000000);
      });
      if (result == VK_TIMEOUT) {
        return m_headless ? fail(result) : REMIXAPI_ERROR_CODE_SUCCESS;
      }
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      setupTime.start();
      m_frames[m_frameIndex].snapshot.reset();
      m_raster.selectFrame(m_frameIndex);
      if (m_particles) m_particles->selectFrame(m_frameIndex);
      if (m_postProcessing) m_postProcessing->selectFrame(m_frameIndex);
      if (m_pathTracer)
        m_pathTracer->selectFrame(m_frameIndex);
      uint32_t imageIndex = 0;
      // Preserve next-presentation capture completion. Event polling stays
      // nonblocking; only a pending screenshot needs the preceding frame.
      m_screenshot.complete(m_device, true);
      setupTime.stop();
      result = measured(p.acquireMilliseconds, [&] {
        if (m_headless)
          return VK_SUCCESS;
        return acquireImage(m_frames[m_frameIndex].imageAvailable, 1000000000, imageIndex);
      });
      if (result == VK_TIMEOUT || result == VK_NOT_READY) {
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
        m_rebuild = true;
        m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return fail(result);
      }
      m_rebuild |= result == VK_SUBOPTIMAL_KHR;
      m_frameStarted = true;
      latencyMarker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, m_latencyRenderFrame);
      queueLock.lock();
      const GeneratedImages generated = m_headless ? GeneratedImages{} : acquireGenerated();
      status = measured(p.recordMilliseconds, [&] {
        return recordFrame(imageIndex, traceScene ? &*traceScene : nullptr, generated);
      });
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return fail();
      }
      result = vkResetFences(m_device, 1, &m_frames[m_frameIndex].fence);
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      const VkPipelineStageFlags waitStage =
          m_headless ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                     : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT;
      // With generated frames: every acquired image, and a semaphore for each present.
      VkSemaphore waits[1 + FrameGeneration::maxGenerated] = {m_headless ? m_sharedReleased
                                                                         : m_frames[m_frameIndex].imageAvailable};
      VkPipelineStageFlags waitStages[1 + FrameGeneration::maxGenerated] = {waitStage};
      VkSemaphore signals[1 + FrameGeneration::maxGenerated] = {m_headless ? m_sharedReady
                                                                           : m_renderFinished[imageIndex]};
      for (uint32_t i = 0; i < generated.count; ++i) {
        waits[1 + i] = m_frames[m_frameIndex].generatedAvailable[i];
        waitStages[1 + i] = waitStage;
        signals[1 + i] = m_renderFinished[generated.index[i]];
      }
      const VkSubmitInfo submit = {
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
          .waitSemaphoreCount = m_headless ? uint32_t(m_sharedPresented) : 1 + generated.count,
          .pWaitSemaphores = waits,
          .pWaitDstStageMask = waitStages,
          .commandBufferCount = 1,
          .pCommandBuffers = &m_commandBuffers[m_frameIndex],
          .signalSemaphoreCount = 1 + generated.count,
          .pSignalSemaphores = signals,
      };
      result = measured(p.submitMilliseconds,
                        [&] { return vkQueueSubmit(m_queue, 1, &submit, m_frames[m_frameIndex].fence); });
      if (result != VK_SUCCESS) {
        return fail(result);
      }
      latencyMarker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, m_latencyRenderFrame);
      queueLock.unlock();
      m_screenshot.submitted(m_frames[m_frameIndex].fence);
      if (m_pathTracer && !m_loading)
        measured(m_frameBreakdown.presentReleaseMilliseconds, [&] { m_pathTracer->submitted(); });
      if (m_headless) {
        // Do not acknowledge until ready is actually signaled. A worker crash
        // after the response cannot leave Godot waiting for an unsubmitted frame.
        result = vkWaitForFences(m_device, 1, &m_frames[m_frameIndex].fence, VK_TRUE, 30000000000ull);
        if (result != VK_SUCCESS)
          return fail(result);
        m_sharedPresented = true;
        presented();
        m_frameIndex = (m_frameIndex + 1) % m_frameCount;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      result = measured(p.queuePresentMilliseconds, [&] { return presentImages(imageIndex, generated); });
      m_frameIndex = (m_frameIndex + 1) % m_frameCount;
      if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        presented();
      if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
          result == VK_ERROR_SURFACE_LOST_KHR) {
        m_rebuild = true;
        m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
      return result == VK_SUCCESS ? REMIXAPI_ERROR_CODE_SUCCESS : fail(result);
    } catch (VkResult result) {
      return fail(result);
    } catch (...) {
      return fail();
    }
  }

  remixapi_ErrorCode RemixNativeRuntime::createFrameResources() {
    const VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_queueFamily,
    };
    if (vkCreateCommandPool(m_device, &pool, nullptr, &m_commandPool) != VK_SUCCESS) {
      return fail();
    }
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (auto &frame : m_frames) {
      auto result = vkCreateSemaphore(m_device, &semaphore, nullptr, &frame.imageAvailable);
      if (result != VK_SUCCESS)
        return fail(result);
      for (auto &generated : frame.generatedAvailable) {
        result = vkCreateSemaphore(m_device, &semaphore, nullptr, &generated);
        if (result != VK_SUCCESS)
          return fail(result);
      }
      const VkFenceCreateInfo unsignalled = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      result = vkCreateFence(m_device, &unsignalled, nullptr, &frame.presented);
      if (result != VK_SUCCESS)
        return fail(result);
      result = vkCreateFence(m_device, &fence, nullptr, &frame.fence);
      if (result != VK_SUCCESS)
        return fail(result);
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void RemixNativeRuntime::prepareFrameGeneration() {
    m_frameGenerationReady = false;
    m_generateCount = 0;
    std::erase_if(m_obsoleteFrameGeneration, [](const auto &p) { return p->job->ready(); });
    if (!m_frameGenerationRequested || !m_presenterFrameGeneration || !m_swapchain || !m_ngx)
      return;
    if (m_frameGenerationBackend != m_appliedFrameGenerationBackend) {
      m_appliedFrameGenerationBackend = m_frameGenerationBackend;
      // A failure was the other backend's: the new swapchain forgets it.
      if (m_frameGenerationFailed) {
        m_rebuild = true;
        m_frameGenerationStatus = "backend changed";
        return;
      }
    }
    if (m_frameGenerationFailed)
      return; // the status says why; a new swapchain (or toggling the setting) tries again
    if (!m_pathTracer || !m_targetBackend || m_backend == 0 || m_loading) {
      m_frameGenerationStatus = "waiting for the path tracer";
      return;
    }
    if (m_swapchainImages.size() < 2 + m_presenterFrameGeneration) {
      m_frameGenerationStatus = "the surface gives too few swapchain images";
      return;
    }
    // The finished frame is copied out of its swapchain image and the
    // generated one into another: the usages screenshots and snapshots need.
    if (!m_screenshotSupported || !m_snapshotSupported) {
      m_frameGenerationStatus = "the swapchain's images cannot be copied from and to";
      return;
    }
    try {
      using Mode = upscale::FrameGenerationMode;
      using Backend = FrameGeneration::Backend;
      const auto mode = m_appliedFrameGenerationBackend;
      // NGX is only asked when DLSS may generate.
      auto capabilities = m_ngx->capabilities();
      if (mode != Mode::Fsr31 && capabilities.sdk && !capabilities.initializationComplete) {
        m_ngx->initializeDevice();
        capabilities = m_ngx->capabilities();
      }
      const auto backend = mode == Mode::Fsr31 || (mode == Mode::Auto && !capabilities.fg) ? Backend::Fsr31
                                                                                           : Backend::Dlss;
      if (backend == Backend::Dlss && !capabilities.fg) {
        m_frameGenerationStatus = capabilities.fgReason.empty() ? "DLSS Frame Generation unavailable" : capabilities.fgReason;
        return;
      }
      const auto upscaling = m_pathTracer->upscalingStatistics();
      if (upscaling.backend == upscale::Backend::Native || !upscaling.render.width || !upscaling.render.height) {
        m_frameGenerationStatus = "needs an upscaler (DLSS or FSR; their native-resolution preset counts)";
        return;
      }
      // The depth the upscaler prepares: reversed by FSR's, the device's by
      // DLSS's (upscale::FrameGenerationInputs::depthInverted). FSR frame
      // generation's shaders are chosen for one of them.
      const bool depthInverted = upscaling.backend == upscale::Backend::Fsr31;
      if (m_frameGeneration && !m_frameGeneration->matches(backend, m_swapchainExtent, m_swapchainFormat,
                                                           upscaling.render, depthInverted)) {
        // Its images may still be in use by frames in flight: the next
        // Present recreates the swapchain, which waits for the device.
        m_rebuild = true;
        m_frameGenerationStatus = m_frameGeneration->backend() != backend ? "backend changed" : "render size changed";
        return;
      }
      if (!m_frameGeneration && backend == Backend::Fsr31) {
        // Its pipelines compile on the worker; this thread would only be
        // told they are not ready (runtime/pipelines.h).
        const auto key = std::to_string(m_swapchainExtent.width) + ":" + std::to_string(m_swapchainExtent.height) +
                         ":" + std::to_string(int(m_swapchainFormat)) + ":" + std::to_string(upscaling.render.width) +
                         ":" + std::to_string(upscaling.render.height) + ":" + std::to_string(depthInverted);
        if (m_frameGenerationPreparation && m_frameGenerationPreparation->key != key) {
          m_pipelines->cancel(m_frameGenerationPreparation->job);
          m_obsoleteFrameGeneration.push_back(std::move(m_frameGenerationPreparation));
        }
        if (!m_frameGenerationPreparation) {
          auto next = std::make_shared<PreparedFrameGeneration>();
          next->key = key;
          const auto physical = m_physicalDevice;
          const auto device = m_device;
          const auto family = m_queueFamily;
          const auto display = m_swapchainExtent, render = upscaling.render;
          const auto format = m_swapchainFormat;
          next->job = m_pipelines->prepare("frame-generation-" + std::to_string(++m_frameGenerationGeneration), [=] {
            next->generation = std::make_unique<FrameGeneration>(Backend::Fsr31, nullptr, physical, device, family,
                                                                 display, format, render, depthInverted);
          });
          m_frameGenerationPreparation = std::move(next);
        }
        if (!m_frameGenerationPreparation->job->ready()) {
          m_frameGenerationStatus = "starting (FSR)";
          return;
        }
        const auto prepared = std::move(m_frameGenerationPreparation);
        prepared->job->check();
        m_frameGeneration = std::move(prepared->generation);
      } else if (m_frameGenerationPreparation) {
        // DLSS became the backend while an FSR object was being made.
        m_pipelines->cancel(m_frameGenerationPreparation->job);
        m_obsoleteFrameGeneration.push_back(std::move(m_frameGenerationPreparation));
      }
      if (!m_frameGeneration)
        m_frameGeneration = std::make_unique<FrameGeneration>(Backend::Dlss, m_ngx, m_physicalDevice, m_device,
                                                              m_queueFamily, m_swapchainExtent, m_swapchainFormat,
                                                              upscaling.render, depthInverted);
      m_frameGenerationReady = m_frameGeneration->ready(m_queue);
      // The swapchain has an image for every frame asked for; a backend that
      // makes fewer leaves the others unused.
      m_generateCount = std::min(m_presenterFrameGeneration,
                                 backend == Backend::Dlss ? std::max(capabilities.fgMultiFrameMax, 1u)
                                                          : FrameGeneration::maxGeneratedBy(backend));
      m_frameGenerationStatus = !m_frameGenerationReady
                                    ? "starting"
                                    : std::string("active (") + m_frameGeneration->name() +
                                          ", " + std::to_string(m_generateCount + 1) + "x)";
    } catch (const std::exception &e) {
      m_frameGenerationFailed = true;
      m_frameGenerationStatus = e.what();
    } catch (VkResult result) {
      if (result == VK_ERROR_DEVICE_LOST)
        throw;
      m_frameGenerationFailed = true;
      m_frameGenerationStatus = "Vulkan error " + std::to_string(int(result));
    }
  }

  GeneratedImages RemixNativeRuntime::acquireGenerated() {
    GeneratedImages generated;
    if (!m_frameGenerationReady || !m_frameGeneration)
      return generated;
    // Bounded waits: without free images the frame is presented with fewer
    // generated frames, or alone (counted: m_ungeneratedFrames). Under FIFO an
    // image comes back only when the display has taken the one before it, so
    // the wait is a share of the frame rather than a fixed few milliseconds.
    const uint64_t wait = uint64_t(std::clamp(m_renderedInterval * 0.25, 4.0, 20.0) * 1e6);
    while (generated.count < m_generateCount) {
      uint32_t index = 0;
      const VkResult result = acquireImage(m_frames[m_frameIndex].generatedAvailable[generated.count], wait, index);
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        m_lastGeneratedAcquire = int(result);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
          m_rebuild = true;
          m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
        }
        break;
      }
      generated.index[generated.count++] = index;
    }
    m_ungeneratedFrames += generated.count < m_generateCount;
    return generated;
  }

  void RemixNativeRuntime::recordFrameGeneration(VkCommandBuffer command, uint32_t realIndex,
                                                 const GeneratedImages &generated) {
    const auto real = m_swapchainImages[realIndex];
    VkImage images[FrameGeneration::maxGenerated] = {};
    for (uint32_t i = 0; i < generated.count; ++i)
      images[i] = m_swapchainImages[generated.index[i]];
    const auto inputs = m_pathTracer && !m_loading ? m_pathTracer->frameGeneration()
                                                   : PathTracer::FrameGenerationInputs{};
    if (inputs.valid) {
      try {
        m_frameGeneration->record(command, real, images, generated.count, inputs);
        m_generatedFrames += generated.count;
      } catch (const std::exception &e) {
        // The images hold the rendered frame again (record()).
        m_frameGenerationFailed = true;
        m_frameGenerationStatus = e.what();
        m_duplicatedFrames += generated.count;
      }
      return;
    }
    // Nothing to interpolate with (a debug view, the upscaler just went away).
    m_frameGeneration->duplicate(command, real, images, generated.count);
    m_duplicatedFrames += generated.count;
  }

  VkResult RemixNativeRuntime::acquireImage(VkSemaphore semaphore, uint64_t timeoutNanoseconds,
                                            uint32_t &imageIndex) {
    // Polled: an acquire that waited inside the driver would hold the
    // swapchain against the presenter, whose presents are what frees images.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeoutNanoseconds);
    for (;;) {
      {
        std::lock_guard lock(m_queueMutex);
        const VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, 0, semaphore, VK_NULL_HANDLE, &imageIndex);
        if (result != VK_NOT_READY && result != VK_TIMEOUT)
          return result;
      }
      if (std::chrono::steady_clock::now() >= deadline)
        return VK_TIMEOUT;
      SDL_DelayNS(200000);
    }
  }

  VkResult RemixNativeRuntime::presentImages(uint32_t realIndex, const GeneratedImages &generated) {
    using Clock = std::chrono::steady_clock;
    auto presentOne = [this](uint32_t index, const void *next = nullptr) {
      const VkPresentInfoKHR present = {
          .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
          .pNext = next,
          .waitSemaphoreCount = 1,
          .pWaitSemaphores = &m_renderFinished[index],
          .swapchainCount = 1,
          .pSwapchains = &m_swapchain,
          .pImageIndices = &index,
      };
      std::lock_guard lock(m_queueMutex);
      return vkQueuePresentKHR(m_queue, &present);
    };
    // The pace of rendered frames, for spreading a frame's images over it.
    const auto now = Clock::now();
    if (m_lastRenderedPresent != Clock::time_point{}) {
      const double elapsed = std::chrono::duration<double, std::milli>(now - m_lastRenderedPresent).count();
      if (elapsed < 250) {
        m_renderedInterval = m_renderedInterval > 0 ? m_renderedInterval * 0.9 + elapsed * 0.1 : elapsed;
        m_logHost.add(int64_t(elapsed * 1000));
      }
      else
        m_renderedInterval = 0; // a pause: no pacing from before it
    }
    m_lastRenderedPresent = now;
    latencyMarker(VK_LATENCY_MARKER_PRESENT_START_NV, m_latencyRenderFrame);
    struct PresentEnd {
      RemixNativeRuntime &runtime;
      ~PresentEnd() { runtime.latencyMarker(VK_LATENCY_MARKER_PRESENT_END_NV, runtime.m_latencyRenderFrame); }
    } presentEnd{*this};
    // Frames are shown in order by the presenter thread, each over the frame
    // step that follows the GPU finishing it. The host is not held for that:
    // when the GPU takes longer than half a frame, the previous frame's
    // rendered image is still to be shown now, and waiting for it here gave
    // the host's frames, which the generated ones interpolate between, the
    // GPU's irregular spacing. Only the frame before the previous one must be
    // through (its frame slot, and so its fence, is this frame's).
    if (m_presenter.started())
      m_presenter.wait(generated.count ? 1 : 0);
    takeDeferredResult();
    m_logWait.add(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count());
    if (!generated.count)
      return presentOne(realIndex);
    // The generated frames show the moments between the previous rendered
    // frame and this one, and the rendered frame follows them, a frame step
    // apart. The steps count from when the GPU has finished the frame: a
    // present waits for that anyway, so presents queued at fixed times after
    // the submission all landed at once whenever the GPU took longer than the
    // steps (a GPU-bound frame rate, the case frame generation is for), and
    // the generated frames were replaced before they were seen.
    // An empty submission's fence signals when the queue's earlier work is done.
    const VkFence done = m_frames[m_frameIndex].presented;
    {
      std::lock_guard lock(m_queueMutex);
      vkResetFences(m_device, 1, &done);
      const VkResult submitted = vkQueueSubmit(m_queue, 0, nullptr, done);
      if (submitted != VK_SUCCESS) {
        for (uint32_t i = 0; i < generated.count; ++i)
          presentOne(generated.index[i]); // hand the images back whatever happened
        presentOne(realIndex);
        return submitted;
      }
    }
    const double interval = std::min(m_renderedInterval, 100.0);
    const double step = interval / (generated.count + 1);
    const auto submitted = Clock::now();
    m_presenter.submit([this, realIndex, generated, interval, step, done, presentOne, submitted] {
      // A frame the GPU had finished before its turn came: the presenter is
      // behind the host (below).
      const bool behind = vkGetFenceStatus(m_device, done) == VK_SUCCESS;
      vkWaitForFences(m_device, 1, &done, VK_TRUE, 2000000000);
      auto finished = Clock::now();
      m_logGpu.add(std::chrono::duration_cast<std::chrono::microseconds>(finished - submitted).count());
      // The GPU's time varies from frame to frame, and showing every frame
      // the moment it is finished shows that variation as uneven motion. A
      // frame finished early is held until (nearly) a frame after the one
      // before it was shown; one that is late is shown at once and moves the
      // timeline. "Nearly" lets the timeline come back after a slow frame.
      // Not while the presenter is behind: the host then waits for it, the
      // interval measured is the presenter's own, and holding frames to it
      // kept both slow for most of a minute after frame generation started.
      if (m_paceClock != Clock::time_point{} && interval > 0 && !behind) {
        const auto next = m_paceClock + std::chrono::duration_cast<Clock::duration>(
                                            std::chrono::duration<double, std::milli>(interval * 0.98));
        if (next > finished && next - finished < std::chrono::duration<double, std::milli>(interval))
          finished = next;
      }
      m_paceClock = finished;
      VkResult worst = VK_SUCCESS;
      Clock::time_point shownSince{};
      // Present metering: the first present says how many images make up the
      // frame and the driver shows them evenly spaced, so the rest follow at
      // once. A frame whose configuration the driver refuses is timed here.
      // Diagnostic (REMIX_FG_PRESENT_METERING=0): always timed here.
      static const bool meteringAllowed = [] {
        const char *value = std::getenv("REMIX_FG_PRESENT_METERING");
        return !value || value[0] != '0';
      }();
      bool metered = m_presentMeteringSupported && meteringAllowed;
      for (uint32_t i = 0; i <= generated.count; ++i) {
        const auto deadline = finished + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double, std::milli>(metered && i ? 0 : step * i));
        const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
        if (left > 0)
          SDL_DelayPrecise(Uint64(left));
        // How far past its time this present starts (the log's worst case).
        const auto late =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - deadline).count();
        for (auto worstLate = m_presentLateMicroseconds.load();
             late > worstLate && !m_presentLateMicroseconds.compare_exchange_weak(worstLate, late);) {
        }
        VkSetPresentConfigNV batch = {.sType = presentConfigType,
                                      .numFramesPerBatch = generated.count + 1};
        const VkResult result =
            presentOne(i < generated.count ? generated.index[i] : realIndex, metered && !i ? &batch : nullptr);
        if (metered && !i) {
          if (batch.presentConfigFeedback) {
            metered = false;
            ++m_meteringRefused;
          } else {
            ++m_meteredFrames;
          }
        }
        if (result != VK_SUCCESS && (worst == VK_SUCCESS || worst == VK_SUBOPTIMAL_KHR))
          worst = result;
        // What each kind of frame was on screen for (the log's averages).
        const auto shown = Clock::now();
        auto elapsed = [&](Clock::time_point since) {
          return std::chrono::duration_cast<std::chrono::microseconds>(shown - since).count();
        };
        if (i) {
          m_generatedShownMicroseconds += elapsed(shownSince);
          ++m_generatedShownCount;
          m_logGenerated.add(elapsed(shownSince));
        } else if (m_renderedShownSince != Clock::time_point{} && elapsed(m_renderedShownSince) < 250000) {
          m_renderedShownMicroseconds += elapsed(m_renderedShownSince);
          ++m_renderedShownCount;
          m_logRendered.add(elapsed(m_renderedShownSince));
        }
        shownSince = shown;
      }
      m_renderedShownSince = shownSince;
      if (worst != VK_SUCCESS)
        m_deferredResult.store(int(worst));
    });
    return VK_SUCCESS;
  }

  void RemixNativeRuntime::finishDeferredPresent() {
    if (!m_presenter.started())
      return;
    m_presenter.wait();
    takeDeferredResult();
  }

  void RemixNativeRuntime::takeDeferredResult() {
    const auto result = VkResult(m_deferredResult.exchange(int(VK_SUCCESS)));
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
      m_rebuild = true;
      m_recreateSurface |= result == VK_ERROR_SURFACE_LOST_KHR;
    } else if (result != VK_SUCCESS) {
      fail(result);
    }
  }

  remixapi_ErrorCode RemixNativeRuntime::recordFrame(uint32_t imageIndex, scene::TraceFrame *traceScene,
                                                     const GeneratedImages &generated) {
    auto buffer = m_commandBuffers[m_frameIndex];
    VkResult result = vkResetCommandBuffer(buffer, 0);
    if (result != VK_SUCCESS) {
      return fail(result);
    }
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(buffer, &begin);
    if (result != VK_SUCCESS) {
      return fail(result);
    }
    // GPU pass timers: this slot's previous frame has completed (its fence);
    // its timestamps are read without waiting before the slot is reused.
    m_gpuTimers.beginFrame(buffer, m_frameIndex, m_gpuTimersEnabled, ++m_recordedFrames,
                           m_pathTracer && !m_loading ? m_pathTracer->renderExtent() : m_swapchainExtent,
                           m_swapchainExtent);
    struct AbandonTimers {
      gpu::GpuTimers &timers;
      ~AbandonTimers() { timers.abandon(); } // no-op after endFrame
    } abandonTimers{m_gpuTimers};
    if (m_headless)
      sharedBarrier(buffer, true);
    // GPU-only targets, histories and acceleration structures stay ordered on
    // the queue. The CPU may record ahead using its completed submission slot.
    const VkMemoryBarrier previousFrame{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                         &previousFrame, 0, nullptr, 0, nullptr);
    auto &breakdown = m_frameBreakdown;
    // The API's scene, or the published frame (asynchronous presentation).
    auto &raster = renderer();
    if (m_pathTracer && !m_loading)
      measured(breakdown.traceBeginMilliseconds, [&] {
        m_pathTracer->beginFrame(m_traceOptions, raster);
        // Queue order puts reconstruction recreated in beginFrame ahead of this frame.
        std::lock_guard lock(m_queueMutex);
        m_pathTracer->finishReconstruction(m_queue);
      });
    raster.setSkyTarget(m_pathTracer ? m_pathTracer->skyPass() : VK_NULL_HANDLE);
    if (!m_loading) {
      gpu::GpuScope scope(buffer, "Raster uploads and skinning");
      raster.prepare(buffer, m_backend != 0);
      breakdown.rasterPrepareMilliseconds = 1; // Replaced by the raster profile.
    }
    if (m_pathTracer && !m_loading) {
      // A newly enabled tracer did not exist during CPU preparation.
      std::optional<scene::TraceFrame> prepared;
      if (!traceScene) {
        prepared = measured(m_framePerformance.sceneMilliseconds,
                            [&] { return m_pathTracer->prepareScene(raster, m_traceOptions); });
        traceScene = &*prepared;
      }
      LightingReturn lightingReturn{m_pathTracer};
      lightingReturn.frame = traceScene;
      // Role CUT (remixapi_InstanceInfoMaskEXT): the coverage that hides
      // traced instances from the camera, ahead of primary visibility.
      if (raster.cutCoverage().count) {
        gpu::GpuScope scope(buffer, "Mask coverage (cut)", m_swapchainExtent);
        raster.recordCutCoverage(buffer);
      }
      // The tracer's passes nest in this range; its own time is what they
      // leave out (barriers, small uploads and readbacks between passes).
      gpu::GpuScope scope(buffer, "Path tracer: barriers, copies, other");
      m_pathTracer->prepare(buffer, raster, m_traceOptions, *traceScene);
    }
    VkClearValue clear[2] = {};
    std::copy(std::begin(m_clearColor), std::end(m_clearColor), clear[0].color.float32);
    clear[1].depthStencil.depth = 1;
    const auto image = m_swapchainImages[imageIndex];
    // HDR10 (runtime/hdr_output.h): the frame's passes, its snapshot, the
    // host passes and screenshots take the float back buffer of this frame's
    // slot, which is encoded into the swapchain image after the developer UI.
    const bool hdr = m_hdrOutput.active();
    const auto target = hdr ? m_hdrOutput.backBuffer(m_frameIndex) : image;
    const auto targetFormat = hdr ? HdrOutput::backBufferFormat : m_swapchainFormat;
    const auto layout =
        m_headless || hdr ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const auto &settings = m_profiles.current();
    const HdrNits nits{settings.hdrPeakNits, settings.hdrPaperWhiteNits, settings.hdrUiNits};
    // The HUD layer and the developer UI at their own brightness
    // (HdrOutput::scaleUi).
    auto drawUiLayer = [&](auto &&draw) {
      if (hdr)
        m_hdrOutput.scaleUi(buffer, 1 / nits.uiScale());
      draw();
      if (hdr)
        m_hdrOutput.scaleUi(buffer, nits.uiScale());
    };
    if (hdr)
      m_hdrOutput.prepare(buffer, m_frameIndex);
    bool capture =
        !m_loading && m_snapshotSupported && !m_snapshotAllocationFailed;
    auto lapStart = std::chrono::steady_clock::now();
    auto lap = [&](double &phase) {
      const auto now = std::chrono::steady_clock::now();
      phase += std::chrono::duration<double, std::milli>(now - lapStart).count();
      lapStart = now;
    };
    if (capture && (!m_lastFrame || !m_lastFrame->matches(targetFormat,
                                                          m_swapchainExtent))) {
      try {
        m_lastFrame = FrameSnapshot::create(
            m_physicalDevice, m_device, targetFormat, m_swapchainExtent);
      } catch (VkResult error) {
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        m_snapshotAllocationFailed = true;
        capture = false;
        SDL_Log("Last-frame capture unavailable (Vulkan %d)", int(error));
      } catch (const std::bad_alloc &) {
        m_snapshotAllocationFailed = true;
        capture = false;
        SDL_Log("Last-frame capture allocation failed");
      }
    }
    const bool restore = m_loading && m_snapshotSupported && bool(m_lastFrame);
    if (capture || restore)
      m_frames[m_frameIndex].snapshot = m_lastFrame;
    if (restore) {
      gpu::GpuScope scope(buffer, "Last-frame snapshot restore", m_swapchainExtent);
      m_lastFrame->restore(buffer, target, m_swapchainExtent, layout);
    }
    lap(breakdown.snapshotMilliseconds);
    // Particle emitters: their compute passes, after the tracer's TLAS.
    const bool particles = simulateParticles(buffer);
    VkRenderPassBeginInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = restore ? m_overlayPass : m_renderPass,
        .framebuffer = hdr ? m_hdrOutput.framebuffer(m_frameIndex) : m_framebuffers[imageIndex],
        .renderArea = {{0, 0}, m_swapchainExtent},
        .clearValueCount = 2,
        .pClearValues = clear,
    };
    // Font uploads are recorded by the backend before the render pass. With
    // asynchronous presentation, Present built the UI frame on its thread.
    if (!m_asyncActive)
      drawUi(VK_NULL_HANDLE);
    lap(breakdown.uiMilliseconds);
    // The raster backend's NUMOS sky records its frame outside the pass.
    const bool rasterSky = !m_loading && !m_pathTracer && recordRasterSky(buffer, raster);
    const bool rasterSkyHides =
        rasterSky && m_profiles.current().sky.gameSky == config::SkySettings::GameSky::Hide;
    {
      // Includes the swapchain pass's clear and, when the GPU runs ahead of
      // presentation, its wait for the acquired image.
      gpu::GpuScope scope(buffer, m_pathTracer && !m_loading ? "Composite and tonemap" : "Raster scene",
                          m_swapchainExtent);
      vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
      // Halo outlines (remixapi_InstanceInfoHaloEXT) composite over the world
      // layers, under the view model and the UI: the frame's layers are split
      // around them. Without halos this is the single pass it always was.
      // Masked overlays (remixapi_InstanceInfoMaskEXT) likewise split the
      // world layer: the coverage passes follow its unmasked draws.
      const uint32_t halos = m_loading ? 0 : raster.haloCount();
      const bool masks = !m_loading && raster.maskRuns() != 0;
      // Frame generation takes the frame as it is before the UI layer.
      const bool hudless = generated.count && m_frameGeneration && !m_loading;
      // Particles draw between the world and the view model (with frame
      // generation, after the frame it takes, as the UI).
      const bool split = halos || masks || hudless || particles || rasterSky;
      // The host's screen-space passes (remixapi_HostPassEXT) take the frame,
      // view model included, between render passes before its UI layer.
      const bool hostPasses = !m_loading && m_snapshotSupported && m_hostPasses.pending();
      // HDR10 draws the UI layer on its own, at the UI brightness.
      const bool hudApart = hostPasses || hdr;
      const int sceneLayers = split ? 1 : hudApart ? 2 : 3;
      const bool skipSky = m_pathTracer != nullptr || rasterSkyHides;
      auto recordHostPasses = [&] {
        gpu::GpuScope hostTime(buffer, "Host passes", m_swapchainExtent);
        // The scene's depth stays in the target's depth attachment.
        vkCmdEndRenderPass(buffer);
        m_hostPasses.record(buffer, m_frameIndex, target, targetFormat, m_swapchainExtent, layout,
                            m_pathTracer ? m_pathTracer->viewDepth() : VK_NULL_HANDLE);
        pass.renderPass = m_resumePass;
        vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
      };
      auto recordHud = [&] {
        gpu::GpuScope overlay(buffer, "Raster overlay (HUD)", m_swapchainExtent);
        drawUiLayer([&] { raster.record(buffer, false, true, nullptr, {}, 3, 3); });
      };
      if (!m_loading && m_pathTracer) {
        m_pathTracer->setDisplayOutput(displayOutput());
        m_pathTracer->record(buffer);
        scope.close();
        // Soft sprite overlays (remixapi_InstanceInfoSpriteEXT) sample the
        // scene's depth, which the composite just wrote to the target's depth
        // attachment at output resolution (whatever traced or rasterized it,
        // upscaled or not): the pass is left for a copy of it.
        if (raster.wantsSceneDepth()) {
          gpu::GpuScope depthTime(buffer, "Scene depth for soft overlays", m_swapchainExtent);
          vkCmdEndRenderPass(buffer);
          raster.captureSceneDepth(buffer);
          pass.renderPass = m_resumePass;
          vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
        }
        gpu::GpuScope overlay(buffer, "Raster overlay (HUD, effects)", m_swapchainExtent);
        raster.record(buffer, false, true, nullptr, {}, 0, sceneLayers, masks ? 1 : 0);
      } else if (!m_loading) {
        raster.record(buffer, false, rasterSkyHides, nullptr, {}, 0, sceneLayers, masks ? 1 : 0);
        scope.close();
        if (rasterSky)
          compositeRasterSky(buffer, raster, pass);
      }
      scope.close();
      if (masks) {
        {
          gpu::GpuScope maskTime(buffer, "Mask coverage", m_swapchainExtent);
          // The scene's depth stays in the target's depth attachment.
          vkCmdEndRenderPass(buffer);
          raster.recordMasks(buffer);
          pass.renderPass = m_resumePass;
          vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
        }
        gpu::GpuScope overlay(buffer, "Raster overlay (masked effects)", m_swapchainExtent);
        raster.record(buffer, false, skipSky, nullptr, {}, 0, 1, 2);
      }
      if (halos) {
        {
          gpu::GpuScope haloTime(buffer, "Halo outlines (mask, blur, composite)", m_swapchainExtent);
          pass.renderPass = m_resumePass;
          for (uint32_t group = 0; group < halos; ++group) {
            // The scene's depth stays in the target's depth attachment.
            vkCmdEndRenderPass(buffer);
            raster.recordHaloMask(buffer, group);
            vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
            raster.recordHaloComposite(buffer, group);
          }
        }
      }
      if (particles && !hudless)
        drawParticles(buffer, pass);
      if (split && !hudless && !hudApart) {
        gpu::GpuScope overlay(buffer, "Raster overlay (view model, HUD)", m_swapchainExtent);
        raster.record(buffer, false, true, nullptr, {}, 2, 3);
      } else if (split) {
        {
          gpu::GpuScope overlay(buffer, "Raster overlay (view model)", m_swapchainExtent);
          raster.record(buffer, false, true, nullptr, {}, 2, 2);
        }
        if (hostPasses)
          recordHostPasses();
        if (hudless) {
          // The scene's depth stays in the target's depth attachment.
          vkCmdEndRenderPass(buffer);
          // HDR10: the features take PQ frames, so the frame without HUD is
          // encoded into the swapchain image first (the finished frame
          // replaces it after the developer UI).
          if (hdr)
            m_hdrOutput.encode(buffer, m_frameIndex, imageIndex, nits.clamped().paperWhite);
          m_frameGeneration->captureHudless(buffer, image);
          pass.renderPass = m_resumePass;
          vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
          if (particles)
            drawParticles(buffer, pass);
        }
        recordHud();
      } else if (hudApart) {
        if (hostPasses)
          recordHostPasses();
        recordHud();
      }
    }
    lap(breakdown.compositeMilliseconds);
    if (capture) {
      gpu::GpuScope scope(buffer, "Last-frame snapshot capture", m_swapchainExtent);
      vkCmdEndRenderPass(buffer);
      m_lastFrame->capture(buffer, target, layout);
      pass.renderPass = m_overlayPass;
      vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    }
    lap(breakdown.snapshotMilliseconds);
    {
      gpu::GpuScope scope(buffer, "Developer UI", m_swapchainExtent);
      drawUiLayer([&] { drawUi(buffer); });
      vkCmdEndRenderPass(buffer);
    }
    if (hdr) {
      gpu::GpuScope scope(buffer, "HDR10 encode (BT.2020 PQ)", m_swapchainExtent);
      m_hdrOutput.encode(buffer, m_frameIndex, imageIndex, nits.clamped().paperWhite);
    }
    lap(breakdown.uiMilliseconds);
    if (generated.count)
      recordFrameGeneration(buffer, imageIndex, generated);
    if (m_screenshotSupported) {
      // Diagnostic (REMIX_FG_SCREENSHOT_GENERATED=1): screenshots take the
      // frame's first generated image instead of the rendered one. With 2,
      // every other capture's: a captured sequence then alternates generated
      // and rendered images, which shows where in time the generated ones are.
      static const int generatedShots = [] {
        const char *value = std::getenv("REMIX_FG_SCREENSHOT_GENERATED");
        return value && (value[0] == '1' || value[0] == '2') ? value[0] - '0' : 0;
      }();
      static uint32_t captures = 0;
      if (m_screenshot.pending())
        ++captures;
      const bool takeGenerated = generatedShots == 1 || (generatedShots == 2 && (captures & 1));
      const uint32_t shown = takeGenerated && generated.count ? generated.index[0] : imageIndex;
      gpu::GpuScope scope(buffer, "Screenshot readback", m_swapchainExtent);
      // HDR10: the back buffer, gamma 2.2 with 1.0 at UI white, clipped to SDR.
      if (hdr)
        m_screenshot.record(m_physicalDevice, m_device, buffer, target, m_swapchainExtent, targetFormat,
                            VK_IMAGE_LAYOUT_GENERAL);
      else
        m_screenshot.record(m_physicalDevice, m_device, buffer, m_swapchainImages[shown], m_swapchainExtent,
                            m_swapchainFormat, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    if (m_headless)
      sharedBarrier(buffer, false);
    m_gpuTimers.endFrame(buffer);
    result = vkEndCommandBuffer(buffer);
    return result == VK_SUCCESS ? REMIXAPI_ERROR_CODE_SUCCESS : fail(result);
  }

  remixapi_ErrorCode RemixNativeRuntime::requestScreenshot(const char *path) {
    if (!m_initialized)
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    // The render thread records and completes readbacks: a request is taken
    // between its frames (rare; waits for the frame in flight).
    if (m_asyncActive)
      waitRenderThread();
    if (m_failed)
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    return m_screenshot.request(path, m_screenshotSupported);
  }

  void RemixNativeRuntime::completeScreenshot() {
    // A busy render thread completes the readback itself before recording
    // the next capture (renderFrame); PumpEvents never waits for it.
    if (busyState())
      return;
    if (m_initialized && !m_failed)
      m_screenshot.complete(m_device);
  }
} // namespace remix
