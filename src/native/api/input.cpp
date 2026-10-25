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

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PumpEvents() {
  std::lock_guard lock(g_apiMutex);
  try {
    const auto status = g_runtime.window().pumpEvents();
    if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
      g_runtime.menuShortcut();
      g_runtime.completeScreenshot();
    }
    return status;
  } catch (const std::bad_alloc &) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PollInputEvents(remixapi_InputEvent *events,
                                                                   uint32_t capacity, uint32_t *outCount) {
  if (!outCount)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  *outCount = 0;
  if (capacity && !events)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  std::lock_guard lock(g_apiMutex);
  return g_runtime.window().pollInputEvents(events, capacity, *outCount);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetWindowMetrics(remixapi_WindowMetrics *outMetrics) {
  if (!outMetrics)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  *outMetrics = {};
  std::lock_guard lock(g_apiMutex);
  return g_runtime.window().getWindowMetrics(*outMetrics);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetTextInputEnabled(remixapi_Bool enabled) {
  std::lock_guard lock(g_apiMutex);
  return g_runtime.window().setTextInputEnabled(enabled != 0);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetWindowState(remixapi_WindowState *outState) {
  if (outState == nullptr) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  std::lock_guard lock(g_apiMutex);
  *outState = {};
  return g_runtime.window().getWindowState(*outState);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PollMouseState(remixapi_MouseState *outState) {
  if (outState == nullptr) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  std::lock_guard lock(g_apiMutex);
  *outState = {};
  return g_runtime.window().pollMouseState(*outState);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_IsKeyDown(remixapi_Key key, remixapi_Bool *outIsDown) {
  if (outIsDown == nullptr) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  std::lock_guard lock(g_apiMutex);
  *outIsDown = 0;
  return g_runtime.window().isKeyDown(key, *outIsDown);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetMouseGrabbed(remixapi_Bool grabbed) {
  std::lock_guard lock(g_apiMutex);
  return g_runtime.window().setMouseGrabbed(grabbed != 0);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetCursorPosition(float x, float y) {
  std::lock_guard lock(g_apiMutex);
  return g_runtime.window().setCursorPosition(x, y);
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetFullscreen(remixapi_Bool fullscreen) {
  std::lock_guard lock(g_apiMutex);
  return g_runtime.window().setFullscreen(fullscreen != 0);
}
}
