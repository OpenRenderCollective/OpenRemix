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
#include "api/exports.h"
#if REMIX_METAL
#include "metal/runtime.h"
#else
#include "runtime/runtime.h"
#endif
#include <mutex>
#include <string>
#include <unordered_map>

namespace remix::api {
  void releaseUsdSubmissions();
  void publishUsdReloads();
  extern remix::RemixNativeRuntime g_runtime;
  extern std::recursive_mutex g_apiMutex;
  extern uint32_t g_frameExitStages;
  extern PFN_remixapi_BridgeCallback g_beginSceneCallback;
  extern PFN_remixapi_BridgeCallback g_endSceneCallback;
  extern PFN_remixapi_BridgeCallback g_presentCallback;
  extern std::unordered_map<std::string, std::string> g_gameValues;

  // The host's time inside resource calls since the last Present, for the
  // frame pacing figures: all of it, and the longest single call.
  inline std::atomic<uint64_t> g_apiCallNanoseconds{0}, g_apiLongestNanoseconds{0};
  inline std::atomic<const char *> g_apiLongestName{""};

  template <typename F> remixapi_ErrorCode resourceCall(F &&call, const char *name = __builtin_FUNCTION()) {
    struct Timed {
      const char *name;
      std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
      ~Timed() {
        const uint64_t elapsed = uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        g_apiCallNanoseconds.fetch_add(elapsed, std::memory_order_relaxed);
        if (elapsed > g_apiLongestNanoseconds.load(std::memory_order_relaxed)) {
          g_apiLongestNanoseconds.store(elapsed, std::memory_order_relaxed);
          g_apiLongestName.store(name, std::memory_order_relaxed);
        }
      }
    } timed{name};
    try {
      std::lock_guard lock(g_apiMutex);
      if (!g_runtime.isInitialized()) {
        return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
      }
      if (g_runtime.hasFailed()) {
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
      call(g_runtime.raster());
      return REMIXAPI_ERROR_CODE_SUCCESS;
    } catch (remixapi_ErrorCode code) {
      return code;
    } catch (...) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
  }

  remixapi_ErrorCode ignoredEffect();
  remixapi_ErrorCode REMIXAPI_CALL requireRenderer();
} // namespace remix::api
