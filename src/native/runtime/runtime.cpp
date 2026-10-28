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

#include "platform/memory_release.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace remix {
  RemixNativeRuntime::~RemixNativeRuntime() {
    // Without a Shutdown: the render thread's frame completes first.
    waitRenderThread();
    finishDeferredPresent();
    m_presenter.stop();
    if (m_asyncActive)
      switchPresentation(false);
    m_renderThread.stop();
    if (m_device && !m_deviceLost) {
      vkDeviceWaitIdle(m_device);
    }
    m_latencySleeper.shutdown(); // before the device and the swapchain it sleeps on go
    destroyVulkan();
    m_window.shutdown();
  }

  remixapi_ErrorCode RemixNativeRuntime::initialize(const remixapi_StartupInfo &info) {
    if (isInitialized()) {
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    }
#if defined(_WIN32)
    if (info.forceNoVkSwapchain) {
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
#endif
    m_headless = m_disableProfiles = m_disableMods = false;
    struct Extension {
      remixapi_StructType type;
      const void *next;
    };
    for (auto p = static_cast<const Extension *>(info.pNext); p;
         p = static_cast<const Extension *>(p->next)) {
      if (p->type != REMIXAPI_STRUCT_TYPE_EDITOR_STARTUP_EXT)
        continue;
      const auto &editor = *reinterpret_cast<const remixapi_EditorStartupEXT *>(p);
      if (editor.version != 1)
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      m_headless = editor.headless;
      m_disableProfiles = editor.disableProfiles;
      m_disableMods = editor.disableMods;
    }
#if !defined(__linux__) && !defined(_WIN32)
    if (m_headless)
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#endif
    m_failed = false;
    m_deviceLost = false;
    m_presentationCount = 0;
    m_lastPresentation = {};
    m_longestPresentationGap = 0;
    m_framePerformance = {.structSize = sizeof(remixapi_FramePerformanceEXT)};
    m_frameIndex = 0;
    const char *frames = std::getenv("REMIX_FRAMES_IN_FLIGHT");
    m_frameCount = frames && std::strcmp(frames, "1") == 0 ? 1u : gpuFrameCount;
    // info.disableSrgbConversionForOutput is accepted but ignored: the output
    // is always gamma 2.2 encoded (UNORM swapchain).
    try {
      if (!m_disableProfiles)
        m_profiles.startup();
      m_profileUi = {};
      m_uiState = REMIXAPI_UI_STATE_NONE;
      if (!m_profiles.message().empty())
        SDL_Log("OpenRemix profiles: %s", m_profiles.message().c_str());
      auto status = m_headless ? REMIXAPI_ERROR_CODE_SUCCESS : m_window.initialize(info);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return status;
      }
      auto finish = [&](remixapi_ErrorCode error) {
        if (error != REMIXAPI_ERROR_CODE_SUCCESS) {
          shutdown();
        }
        return error;
      };
      status = createInstance();
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      status = m_headless ? REMIXAPI_ERROR_CODE_SUCCESS
                          : m_window.createVulkanSurface(m_instance, nullptr, m_surface);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      QueueSelection selection;
      status = selectPhysicalDevice(selection);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      status = createDevice(selection);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      applySettings();
      m_raster.initializeGpu(m_physicalDevice, m_device, m_textureCompression, m_memoryBudget);
      status = createFrameResources();
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      if (!m_headless && !initializeUi()) {
        return finish(REMIXAPI_ERROR_CODE_GENERAL_FAILURE);
      }
      status = m_headless ? REMIXAPI_ERROR_CODE_SUCCESS : recreateSwapchain();
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return finish(status);
      }
      m_initialized = true;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    } catch (...) {
      shutdown();
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
  }

  remixapi_ErrorCode RemixNativeRuntime::shutdown() {
    if (m_window.handle() && m_window.checkThread() == REMIXAPI_ERROR_CODE_WRONG_THREAD) {
      return REMIXAPI_ERROR_CODE_WRONG_THREAD;
    }
    // Asynchronous presentation: the render thread finishes its frame, its GPU
    // state returns to the API's renderer and the thread is joined before
    // anything it uses is released.
    waitRenderThread();
    finishDeferredPresent();
    m_presenter.stop();
    if (m_asyncActive)
      switchPresentation(false);
    m_renderThread.stop();
    m_frameScene.reset();
    m_asyncCaller = m_previousAsyncCaller = m_asyncRender = {};
    m_renderedMemory = {};
    if (m_device && !m_deviceLost) {
      vkDeviceWaitIdle(m_device);
    }
    m_latencySleeper.shutdown(); // before the device and the swapchain it sleeps on go
    destroyVulkan();
    m_window.shutdown();
    m_initialized = false;
    m_frameStarted = false;
    m_failed = false;
    m_deviceLost = false;
    m_recreateSurface = false;
    m_rebuild = false;
    m_vsync = false;
    m_backend = m_requestedBackend = 0;
    m_traceOptions = {};
    if (!m_disableProfiles)
      m_profiles.savePanel(m_profiles.panel());
    m_profiles = {};
    m_appliedSettings = {};
    m_profileUi = {};
    m_backendFailed = false;
    m_targetBackend = 0;
    m_sharcReset = 0;
    const auto defaults = config::Settings{};
    std::copy(defaults.clearColor.begin(), defaults.clearColor.end(), m_clearColor);
    m_input[0] = 0;
    m_traceSupported = false;
    m_denoiseCapabilities = {.structSize = sizeof(remixapi_DenoisingCapabilitiesEXT)};
    m_uiState = REMIXAPI_UI_STATE_NONE;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::fail(VkResult result) {
    m_failed = true;
    m_deviceLost |= result == VK_ERROR_DEVICE_LOST;
    SDL_Log("Remix Vulkan failure: %d", static_cast<int>(result));
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  void RemixNativeRuntime::destroyVulkan() {
    if (m_pipelines)
      m_pipelines->stop();
    if (m_pipelines) {
      const auto s = m_pipelines->statistics();
      SDL_Log(
          "OpenRemix pipelines: created=%llu reused=%llu failed=%llu "
          "compile_ms=%.3f cache_hits=%llu "
          "driver_ns=%llu presentations=%llu longest_presentation_gap_ms=%.3f",
          (unsigned long long)s.created, (unsigned long long)s.reused,
          (unsigned long long)s.failed, s.milliseconds,
          (unsigned long long)s.cacheHits,
          (unsigned long long)s.driverNanoseconds,
          (unsigned long long)m_presentationCount, m_longestPresentationGap);
    }
    // Join before releasing staging objects, immutable layouts or the device.
    m_preparation.reset();
    m_obsoletePreparations.clear();
    m_postPreparation.reset();
    m_obsoletePost.clear();
    m_frameGenerationPreparation.reset();
    m_obsoleteFrameGeneration.clear();
    destroySwapchain();
    destroySharedFrame();
    m_lastFrame.reset();
    m_snapshotSupported = m_snapshotAllocationFailed = false;
    m_screenshot.reset(m_device);
    shutdownUi();
    m_pathTracer.reset();
    m_rasterSky.reset();
    m_rasterSkyReady = false;
    m_postProcessing.reset();
    m_exposureState.reset();
    m_postFailure.clear();
    m_failedPostKey.clear();
    m_ngx.reset();
    m_raster.shutdownGpu();
    m_hostPasses.destroy();
    m_particles.reset();
    m_particleFrame = {};
    m_particleClock = {};
    m_pipelines.reset();
    m_activePreparationKey.clear();
    m_failedPreparationKey.clear();
    m_preparationFailure.clear();
    m_loading = m_rasterPreparing = false;
    m_localSourcesSeen = false;
    m_reconstructionFailureMask = 0;
    m_reconstructionFailureKey.clear();
    m_reconstructionFailure.clear();
    if (m_device) {
      for (auto &frame : m_frames) {
        if (frame.fence)
          vkDestroyFence(m_device, frame.fence, nullptr);
        if (frame.imageAvailable)
          vkDestroySemaphore(m_device, frame.imageAvailable, nullptr);
        for (auto semaphore : frame.generatedAvailable)
          if (semaphore)
            vkDestroySemaphore(m_device, semaphore, nullptr);
        if (frame.presented)
          vkDestroyFence(m_device, frame.presented, nullptr);
        frame = {};
      }
      if (m_commandPool) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
      }
      if (m_latencySemaphore)
        vkDestroySemaphore(m_device, m_latencySemaphore, nullptr);
      m_latencySemaphore = VK_NULL_HANDLE;
      m_lowLatencySupported = m_lowLatencyActive = false;
      m_gpuTimers.shutdown();
      MemoryRelease::drain();
      vkDestroyDevice(m_device, nullptr);
    }
    if (m_surface) {
      vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    }
    if (m_instance) {
      vkDestroyInstance(m_instance, nullptr);
    }
    m_commandPool = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_queue = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_surface = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
  }
} // namespace remix
