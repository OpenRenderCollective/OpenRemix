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

#include "platform/window.h"

#include <algorithm>
#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace remix {

  RemixSdlWindow::~RemixSdlWindow() { shutdown(); }

  remixapi_ErrorCode RemixSdlWindow::initialize(const remixapi_StartupInfo &info) {
#if defined(__APPLE__)
    if (!pthread_main_np()) {
      m_lastError = "macOS window startup must run on the main thread";
      return REMIXAPI_ERROR_CODE_WRONG_THREAD;
    }
#endif
    if (m_window != nullptr) {
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    }

    m_ownerThread = std::this_thread::get_id();
#if defined(_WIN32)
    // A host-owned window lives on the host's message thread. SDL's pump would
    // otherwise PeekMessage(NULL, PM_REMOVE) every message of that thread,
    // taking them from the host's own loop. Without its loop, SDL only drains
    // the events its window procedure already queued.
    if (info.hwnd) {
      SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "0");
      m_embedded = true;
    }
#endif
    if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
      if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        setSdlError("SDL_InitSubSystem");
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
      m_ownsVideoSubsystem = true;
    }

#if defined(_WIN32)
    if (info.hwnd) {
      SDL_PropertiesID props = SDL_CreateProperties();
      if (props) {
        if (SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, info.hwnd) &&
            SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true)) {
          m_window = SDL_CreateWindowWithProperties(props);
        }
        SDL_DestroyProperties(props);
      }
    } else {
      m_window = SDL_CreateWindow("Remix Shell", 1280, 720,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    }
#else
    const char *title =
        info.windowTitle != nullptr && info.windowTitle[0] != '\0' ? info.windowTitle : "Remix Shell";
    const int width = static_cast<int>(info.windowWidth != 0 ? info.windowWidth : 1280u);
    const int height = static_cast<int>(info.windowHeight != 0 ? info.windowHeight : 720u);
#if REMIX_METAL
    SDL_WindowFlags flags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    if (info.fullscreen) {
      flags |= SDL_WINDOW_FULLSCREEN;
    }

    m_window = SDL_CreateWindow(title, width, height, flags);
#endif
    if (m_window == nullptr) {
      setSdlError("SDL_CreateWindow");
      shutdown();
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    m_closeRequested = false;
    m_appInBackground = false;
    m_relativeX = 0.0f;
    m_relativeY = 0.0f;
    m_wheelX = 0.0f;
    m_wheelY = 0.0f;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void RemixSdlWindow::shutdown() {
#if REMIX_METAL
    if (m_metalView != nullptr) {
      SDL_Metal_DestroyView(m_metalView);
      m_metalView = nullptr;
    }
#endif
    if (m_window != nullptr) {
      SDL_DestroyWindow(m_window);
      m_window = nullptr;
    }

    if (m_ownsVideoSubsystem) {
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      m_ownsVideoSubsystem = false;
    }

    m_ownerThread = std::thread::id{};
    m_embedded = false;
    m_eventHandler = nullptr;
    m_eventUserData = nullptr;
    m_surfaceWindow = nullptr;
    m_guiWantsKeyboard = false;
    m_guiWantsMouse = false;
    m_gameWantsText = false;
    m_guiWantsText = false;
    m_startedTextInput = false;
    m_inputEvents.clear();
  }

  bool RemixSdlWindow::hdrEnabled() const {
    return m_window && SDL_GetBooleanProperty(SDL_GetWindowProperties(m_window), SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN,
                                              false);
  }

  remixapi_ErrorCode RemixSdlWindow::getWindowState(remixapi_WindowState &state) const {
    remixapi_ErrorCode threadStatus = checkPresentThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(m_window, &width, &height)) {
      setSdlError("SDL_GetWindowSizeInPixels");
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    const SDL_WindowFlags flags = SDL_GetWindowFlags(m_window);
    state.focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
    state.closeRequested = m_closeRequested;
    state.minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
    state.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
    state.guiWantsKeyboard = m_guiWantsKeyboard;
    state.guiWantsMouse = m_guiWantsMouse;
    const bool drawable = isSurfaceAvailable() && !m_appInBackground &&
                          (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) == 0;
    state.drawableWidth = drawable ? static_cast<uint32_t>(std::max(width, 0)) : 0;
    state.drawableHeight = drawable ? static_cast<uint32_t>(std::max(height, 0)) : 0;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixSdlWindow::setFullscreen(bool fullscreen) {
    remixapi_ErrorCode threadStatus = checkThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    if (!SDL_SetWindowFullscreen(m_window, fullscreen)) {
      setSdlError("SDL_SetWindowFullscreen");
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

#if REMIX_METAL
  void *RemixSdlWindow::metalLayer() {
    if (checkThread() != REMIXAPI_ERROR_CODE_SUCCESS)
      return nullptr;
    if (m_metalView == nullptr) {
      m_metalView = SDL_Metal_CreateView(m_window);
      if (m_metalView == nullptr) {
        setSdlError("SDL_Metal_CreateView");
        return nullptr;
      }
    }
    return SDL_Metal_GetLayer(m_metalView);
  }
#else
  std::vector<const char *> RemixSdlWindow::getVulkanInstanceExtensions() const {
    uint32_t count = 0;
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr) {
      setSdlError("SDL_Vulkan_GetInstanceExtensions");
      return {};
    }

    return std::vector<const char *>(extensions, extensions + count);
  }
#endif

  bool RemixSdlWindow::isSurfaceAvailable() const {
#if defined(__ANDROID__)
    return SDL_GetPointerProperty(SDL_GetWindowProperties(m_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                  nullptr) != nullptr;
#else
    return true;
#endif
  }

  bool RemixSdlWindow::isVulkanSurfaceCurrent() const {
#if defined(__ANDROID__)
    return m_surfaceWindow == SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                                     SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#else
    return true;
#endif
  }

#if !REMIX_METAL
  remixapi_ErrorCode RemixSdlWindow::createVulkanSurface(VkInstance instance,
                                                         const VkAllocationCallbacks *allocator,
                                                         VkSurfaceKHR &surface) const {
    remixapi_ErrorCode threadStatus = checkPresentThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    if (!SDL_Vulkan_CreateSurface(m_window, instance, allocator, &surface)) {
      setSdlError("SDL_Vulkan_CreateSurface");
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

#if defined(__ANDROID__)
    m_surfaceWindow = SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                             SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#endif
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }
#endif

  remixapi_ErrorCode RemixSdlWindow::getWindowMetrics(remixapi_WindowMetrics &metrics) const {
    metrics = {};
    const auto status = checkPresentThread();
    if (status != REMIXAPI_ERROR_CODE_SUCCESS)
      return status;
    int width, height, pixelsX, pixelsY;
    if (!SDL_GetWindowSize(m_window, &width, &height) ||
        !SDL_GetWindowSizeInPixels(m_window, &pixelsX, &pixelsY)) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    metrics = {static_cast<uint32_t>(std::max(0, width)), static_cast<uint32_t>(std::max(0, height)),
               static_cast<uint32_t>(std::max(0, pixelsX)), static_cast<uint32_t>(std::max(0, pixelsY))};
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixSdlWindow::checkThread() const {
    if (m_window == nullptr) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    return m_ownerThread == std::this_thread::get_id() ? REMIXAPI_ERROR_CODE_SUCCESS
                                                       : REMIXAPI_ERROR_CODE_WRONG_THREAD;
  }

  remixapi_ErrorCode RemixSdlWindow::checkPresentThread() const {
    if (m_window == nullptr) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    return m_embedded ? REMIXAPI_ERROR_CODE_SUCCESS : checkThread();
  }

  void RemixSdlWindow::setSdlError(const char *operation) const {
    m_lastError = operation;
    m_lastError += ": ";
    m_lastError += SDL_GetError();
  }

} // namespace remix
