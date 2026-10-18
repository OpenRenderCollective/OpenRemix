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

#include "api/state.h"
#include "api/entrypoints.h"
#include <remix/remix_reconstruction_ext.h>
#include <algorithm>
#include <cstring>

using namespace remix::api;

extern "C" {
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetReconstructionDiagnosticsEXT(remixapi_ReconstructionDiagnosticsEXT *out) {
  if (!out || out->structSize < 8) return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized()) return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    const auto stats = g_runtime.reconstructionStatistics();
    remixapi_ReconstructionDiagnosticsEXT result{};
    result.structSize = sizeof(result); result.version = 1;
    result.capabilities = uint32_t(stats.sdkAvailable) | uint32_t(stats.srSupported) << 1 | uint32_t(stats.rrSupported) << 2;
    result.requestedMode = uint32_t(stats.requested); result.activeBackend = uint32_t(stats.backend);
    result.renderWidth = stats.render.width; result.renderHeight = stats.render.height;
    result.displayWidth = stats.display.width; result.displayHeight = stats.display.height;
    result.evaluatedFrames = stats.frames; result.historyResets = stats.resets;
    result.failures = stats.failures; result.memoryBytes = stats.memoryBytes;
    result.gpuMilliseconds = stats.milliseconds;
    std::strncpy(result.fallbackReason, stats.failure.c_str(), sizeof(result.fallbackReason)-1);
    std::memcpy(out, &result, std::min(size_t(out->structSize), sizeof(result)));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) { return REMIXAPI_ERROR_CODE_GENERAL_FAILURE; }
}


REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RequestPresentedScreenshot(const char *absolutePath) {
  if (absolutePath == nullptr || absolutePath[0] == '\0') {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  try {
    std::lock_guard lock(g_apiMutex);
    return g_runtime.requestScreenshot(absolutePath);
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetPresentationCount(uint64_t *outCount) {
  if (!outCount)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  *outCount = 0;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *outCount = g_runtime.presentationCount();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetFeatureAvailability(remixapi_FeatureAvailability *outFeatures) {
  if (outFeatures == nullptr) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  try {
    std::lock_guard lock(g_apiMutex);
    *outFeatures = g_runtime.getFeatureAvailability();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetBackendCapabilitiesEXT(remixapi_BackendCapabilitiesEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.backendCapabilities();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetFramePerformanceEXT(remixapi_FramePerformanceEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.framePerformance();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetFrameBreakdownEXT(remixapi_FrameBreakdownEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  *out = g_runtime.frameBreakdown();
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

static_assert(sizeof(remixapi_TextureStatisticsEXT) == 168);

// Process-wide counters: no runtime state is read, so no API lock is taken.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetTextureStatisticsEXT(remixapi_TextureStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  const auto &c = remix::scene::textureCounters;
  auto count = [](const std::atomic<uint64_t> &value) { return value.load(std::memory_order_relaxed); };
  auto milliseconds = [&](const std::atomic<uint64_t> &ticks) { return double(count(ticks)) * 1e-6; };
  *out = {.structSize = sizeof(*out)};
  out->texturesCreated = count(c.texturesCreated);
  out->texturesDestroyed = count(c.texturesDestroyed);
  out->materialsCreated = count(c.materialsCreated);
  out->materialsDestroyed = count(c.materialsDestroyed);
  out->updateCalls = count(c.updateCalls);
  out->updates = count(c.updates);
  out->updateBytes = count(c.updateBytes);
  out->updateMilliseconds = milliseconds(c.updateTicks);
  out->uploads = count(c.uploads);
  out->uploadBytes = count(c.uploadBytes);
  out->uploadMilliseconds = milliseconds(c.uploadTicks);
  out->gpuTexturesCreated = count(c.gpuTexturesCreated);
  out->gpuTexturesDestroyed = count(c.gpuTexturesDestroyed);
  out->gpuMaterialsCreated = count(c.gpuMaterialsCreated);
  out->gpuMaterialsDestroyed = count(c.gpuMaterialsDestroyed);
  out->gpuCreateBytes = count(c.gpuCreateBytes);
  out->gpuCreateMilliseconds = milliseconds(c.gpuCreateTicks);
  out->gpuDestroyMilliseconds = milliseconds(c.gpuDestroyTicks);
  out->emissionRefreshes = count(c.emissionRefreshes);
  out->emissionRefreshMilliseconds = milliseconds(c.emissionRefreshTicks);
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

static_assert(sizeof(remixapi_GpuPassTimingEXT) == 88 && sizeof(remixapi_GpuPassTimingsEXT) == 80 + 88 * 128);
#if !REMIX_METAL // The Metal backend records no Vulkan pass timers.
static_assert(REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT >= remix::gpu::GpuTimers::maxPasses);
#endif

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetGpuPassTimingsEXT(remixapi_GpuPassTimingsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  const auto &t = g_runtime.gpuPassTimings();
  *out = {.structSize = sizeof(*out)};
  out->available = t.available;
  out->truncated = t.truncated;
  out->queueFamily = t.queueFamily;
  out->timestampValidBits = t.timestampValidBits;
  out->frame = t.frame;
  out->frameMilliseconds = t.frameMilliseconds;
  out->passMilliseconds = t.passMilliseconds;
  out->untimedMilliseconds = t.untimedMilliseconds;
  out->timestampPeriod = t.timestampPeriod;
  out->renderWidth = t.renderWidth;
  out->renderHeight = t.renderHeight;
  out->outputWidth = t.outputWidth;
  out->outputHeight = t.outputHeight;
  const size_t count = std::min<size_t>(t.passes.size(), REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT);
  out->passCount = uint32_t(count);
  out->truncated += uint32_t(t.passes.size() - count);
  for (size_t i = 0; i < count; ++i) {
    const auto &in = t.passes[i];
    auto &pass = out->passes[i];
    std::memcpy(pass.name, in.name, sizeof(pass.name));
    pass.width = in.width;
    pass.height = in.height;
    pass.depth = in.depth;
    pass.parent = in.parent;
    pass.milliseconds = in.milliseconds;
    pass.selfMilliseconds = in.selfMilliseconds;
  }
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetTracingStatisticsEXT(remixapi_TracingStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.tracingStatistics();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetLocalLightStatisticsEXT(remixapi_LocalLightStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.localLightStatistics();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetLocalLightSetStatisticsEXT(remixapi_LocalLightSetStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  *out = g_runtime.localLightSetStatistics();
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetCameraCullingStatisticsEXT(remixapi_CameraCullingStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  *out = g_runtime.cameraCullingStatistics();
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetLocalLightGuidingStatisticsEXT(remixapi_LocalLightGuidingStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.localLightGuidingStatistics();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetShadowStatisticsEXT(remixapi_ShadowStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.shadowStatistics();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetSharcStatisticsEXT(remixapi_SharcStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.sharcStatistics();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetDenoisingCapabilitiesEXT(remixapi_DenoisingCapabilitiesEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.denoisingCapabilities();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetDenoisingStatisticsEXT(remixapi_DenoisingStatisticsEXT *out) {
  if (!out || out->structSize != sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = g_runtime.denoisingStatistics();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
}
