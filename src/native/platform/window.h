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

#include <string>
#include <thread>
#include <vector>
#include <deque>

#include <SDL3/SDL.h>
#if !REMIX_METAL
#include <SDL3/SDL_vulkan.h>
#endif

#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>

namespace remix {

  class RemixSdlWindow {
  public:
    using EventHandler = bool (*)(const SDL_Event *event, void *userData);

    RemixSdlWindow() = default;
    ~RemixSdlWindow();

    RemixSdlWindow(const RemixSdlWindow &) = delete;
    RemixSdlWindow &operator=(const RemixSdlWindow &) = delete;

    remixapi_ErrorCode initialize(const remixapi_StartupInfo &info);
    void shutdown();

    remixapi_ErrorCode pumpEvents();
    remixapi_ErrorCode getWindowState(remixapi_WindowState &state) const;
    remixapi_ErrorCode pollMouseState(remixapi_MouseState &state);
    remixapi_ErrorCode isKeyDown(remixapi_Key key, remixapi_Bool &isDown) const;
    remixapi_ErrorCode setMouseGrabbed(bool grabbed);
    remixapi_ErrorCode setCursorPosition(float x, float y);
    remixapi_ErrorCode setFullscreen(bool fullscreen);
    remixapi_ErrorCode pollInputEvents(remixapi_InputEvent *events, uint32_t capacity, uint32_t &count);
    remixapi_ErrorCode getWindowMetrics(remixapi_WindowMetrics &metrics) const;
    remixapi_ErrorCode setTextInputEnabled(bool enabled);
    void syncTextInput(bool guiWantsText);

#if REMIX_METAL
    // The window's CAMetalLayer (a CAMetalLayer *), from a view created on
    // first use and destroyed with the window.
    void *metalLayer();
#else
    std::vector<const char *> getVulkanInstanceExtensions() const;
    remixapi_ErrorCode createVulkanSurface(VkInstance instance, const VkAllocationCallbacks *allocator,
                                           VkSurfaceKHR &surface) const;
#endif

    void setEventHandler(EventHandler handler, void *userData);
    void setGuiCapture(bool keyboard, bool mouse);

    // The developer panel's shortcut (config::PanelState): a key and exactly the
    // modifiers held with it. A press toggles the panel (takeMenuToggle); it, its
    // repeats, its release and any text it types reach neither ImGui nor the host.
    enum MenuModifier : uint32_t { MenuAlt = 1, MenuCtrl = 2, MenuShift = 4, MenuGui = 8 };
    void setMenuShortcut(remixapi_Key key, uint32_t modifiers);
    // The next key pressed, with the modifiers held, becomes the shortcut
    // (takeCapturedMenuShortcut); Escape keeps the current one.
    void captureMenuShortcut() { m_capturingMenu = true; }
    void cancelMenuShortcutCapture() { m_capturingMenu = false; }
    bool capturingMenuShortcut() const { return m_capturingMenu; }
    bool takeCapturedMenuShortcut(remixapi_Key &key, uint32_t &modifiers);
    // Whether the shortcut was pressed an odd number of times since the last call.
    bool takeMenuToggle();
    static std::string shortcutName(remixapi_Key key, uint32_t modifiers);
    void requestClose() { m_closeRequested = true; }

    SDL_Window *handle() const { return m_window; }
    // The window's display has HDR on (Windows HDR; SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN).
    bool hdrEnabled() const;
    // True when Startup wrapped a host-owned window (StartupInfo::hwnd).
    bool embedded() const { return m_embedded; }
    // Whether the caller is the thread that started the window: SDL's events,
    // input and the window's destruction belong to it.
    remixapi_ErrorCode checkThread() const;

    bool isSurfaceAvailable() const;
    bool isVulkanSurfaceCurrent() const;

    const std::string &lastError() const { return m_lastError; }

  private:
    // What Present needs of the window (its state, its size, a surface). A
    // host-owned window's messages are the host's, on whichever thread it
    // likes: a host that renders on a thread of its own presents from that
    // one, and from the starting thread again when it stops. The API mutex
    // keeps the calls apart.
    remixapi_ErrorCode checkPresentThread() const;
    void setSdlError(const char *operation) const;
    static SDL_Scancode toScancode(remixapi_Key key);
    static remixapi_Key fromScancode(SDL_Scancode scancode);
    // Whether the event belongs to the panel's shortcut (and is kept from ImGui
    // and the host).
    bool menuShortcut(const SDL_Event &event);
    static uint32_t toMouseButtons(SDL_MouseButtonFlags buttons);
    void queueInput(const remixapi_InputEvent &event);
    void processInput(const SDL_Event &event);

    static bool SDLCALL eventFilter(void *userdata, SDL_Event *event);

    SDL_Window *m_window = nullptr;
    std::thread::id m_ownerThread;
    bool m_ownsVideoSubsystem = false;
    bool m_embedded = false;
    bool m_adoptedWindow = false;
    bool m_appInBackground = false;
    mutable void *m_surfaceWindow = nullptr;
#if REMIX_METAL
    SDL_MetalView m_metalView = nullptr;
#endif
    bool m_closeRequested = false;
    bool m_guiWantsKeyboard = false;
    bool m_guiWantsMouse = false;
    bool m_gameWantsText = false;
    bool m_guiWantsText = false;
    // Text input this window started: a host-owned (embedded) window's own
    // text input, such as a game console's, is the host's to stop.
    bool m_startedTextInput = false;
    std::deque<remixapi_InputEvent> m_inputEvents;
    float m_relativeX = 0.0f;
    float m_relativeY = 0.0f;
    float m_wheelX = 0.0f;
    float m_wheelY = 0.0f;
    EventHandler m_eventHandler = nullptr;
    void *m_eventUserData = nullptr;
    SDL_Scancode m_menuScancode = SDL_SCANCODE_X;
    uint32_t m_menuModifiers = MenuAlt;
    // The shortcut's key while it is held, and whether its press may type next.
    SDL_Scancode m_swallowedKey = SDL_SCANCODE_UNKNOWN;
    bool m_swallowText = false;
    bool m_capturingMenu = false, m_menuCaptured = false;
    remixapi_Key m_capturedKey{};
    uint32_t m_capturedModifiers = 0;
    uint32_t m_menuToggles = 0;
    mutable std::string m_lastError;
  };

} // namespace remix
