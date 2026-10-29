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
#include <cstring>

namespace remix::api {
  remixapi_ErrorCode REMIXAPI_CALL setConfigVariable(const char *key, const char *value) {
    if (!key || !*key || !value) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized()) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    if (std::strcmp(key, "rtx.native.backend") == 0)
      return g_runtime.selectBackend(value);
    if (std::strncmp(key, "rtx.native.pathtrace.", 21) == 0)
      return g_runtime.configureTracing(key + 21, value);
    const bool clearColor = std::strlen(key) == 23 && std::strncmp(key, "rtx.native.clearColor.", 22) == 0 &&
                            std::strchr("rgba", key[22]);
    if (std::strcmp(key, "rtx.native.vsync") == 0 || clearColor ||
        std::strcmp(key, "rtx.native.textures.keepCompressed") == 0 ||
        std::strcmp(key, "rtx.native.capture.retainTextureData") == 0 || std::strcmp(key, "rtx.zUp") == 0 ||
        std::strcmp(key, "rtx.native.vramStatistics") == 0 ||
        std::strcmp(key, "rtx.native.meshes.refreshInPlace") == 0 ||
        std::strcmp(key, "rtx.native.hostPasses") == 0 ||
        std::strcmp(key, "rtx.native.scene.persistentStatic") == 0 ||
        std::strcmp(key, "rtx.native.scene.persistentStaticValidation") == 0 ||
        std::strcmp(key, "rtx.native.asyncPresent") == 0 ||
        std::strcmp(key, "rtx.native.frameGeneration") == 0 ||
        std::strcmp(key, "rtx.native.frameGenerationBackend") == 0 ||
        std::strcmp(key, "rtx.native.frameGenerationFrames") == 0 ||
        std::strcmp(key, "rtx.native.lowLatency") == 0 ||
        std::strcmp(key, "rtx.native.gpuPassTimers") == 0 || std::strncmp(key, "rtx.native.sky.", 15) == 0 ||
        std::strncmp(key, "rtx.native.particles.", 21) == 0 ||
        std::strncmp(key, "rtx.atmosphere.", 15) == 0)
      return g_runtime.configureSetting(key, value);
    // Recognized legacy renderer namespace: accepted but ignored by the unlit renderer.
    return std::strncmp(key, "rtx.", 4) == 0 ? REMIXAPI_ERROR_CODE_SUCCESS
                                           : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
  }

} // namespace remix::api

using namespace remix::api;

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetConfigDefaultEXT(const char *key, const char *value) {
  if (!key || !*key || !value)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  if (std::strncmp(key, "rtx.native.", 11) != 0)
    // Legacy renderer namespaces: accepted and ignored, as SetConfigVariable.
    return std::strncmp(key, "rtx.", 4) == 0 ? REMIXAPI_ERROR_CODE_SUCCESS
                                           : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
  return g_runtime.configureDefault(key, value);
}
