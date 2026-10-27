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

  remixapi_ErrorCode RemixSdlWindow::pumpEvents() {
    remixapi_ErrorCode threadStatus = checkThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    if (m_adoptedWindow) {
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (menuShortcut(event))
        continue;
      if (m_eventHandler != nullptr) {
        m_eventHandler(&event, m_eventUserData);
      }
      processInput(event);

#if defined(__ANDROID__)
      if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND || event.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        m_appInBackground = true;
        m_surfaceWindow = nullptr;
      }
      if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        m_appInBackground = false;
      }
#endif
      if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                           event.window.windowID == SDL_GetWindowID(m_window))) {
        m_closeRequested = true;
      } else if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.windowID == SDL_GetWindowID(m_window)) {
        m_relativeX += event.motion.xrel;
        m_relativeY += event.motion.yrel;
      } else if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.windowID == SDL_GetWindowID(m_window)) {
        m_wheelX += event.wheel.x;
        m_wheelY += event.wheel.y;
      }
    }

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  bool SDLCALL RemixSdlWindow::eventFilter(void *userdata, SDL_Event *event) {
    auto *self = static_cast<RemixSdlWindow *>(userdata);
    if (!self || !event) {
      return true;
    }
    if (self->menuShortcut(*event)) {
      return false;
    }

    if (event->type == SDL_EVENT_QUIT || (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                          event->window.windowID == SDL_GetWindowID(self->m_window))) {
      self->m_closeRequested = true;
    } else if (event->type == SDL_EVENT_MOUSE_MOTION && event->motion.windowID == SDL_GetWindowID(self->m_window)) {
      self->m_relativeX += event->motion.xrel;
      self->m_relativeY += event->motion.yrel;
    } else if (event->type == SDL_EVENT_MOUSE_WHEEL && event->wheel.windowID == SDL_GetWindowID(self->m_window)) {
      self->m_wheelX += event->wheel.x;
      self->m_wheelY += event->wheel.y;
    }

    bool handled = false;
    if (self->m_eventHandler != nullptr) {
      handled = self->m_eventHandler(event, self->m_eventUserData);
    }
    self->processInput(*event);

    if (self->m_adoptedWindow && handled && (self->m_guiWantsMouse || self->m_guiWantsKeyboard)) {
      return false;
    }
    return true;
  }

  void RemixSdlWindow::setEventHandler(EventHandler handler, void *userData) {
    if (m_adoptedWindow) {
      if (m_eventHandler == nullptr && handler != nullptr) {
        SDL_AddEventWatch(eventFilter, this);
      } else if (m_eventHandler != nullptr && handler == nullptr) {
        SDL_RemoveEventWatch(eventFilter, this);
      }
    }
    m_eventHandler = handler;
    m_eventUserData = userData;
  }

  void RemixSdlWindow::setGuiCapture(bool keyboard, bool mouse) {
    if (keyboard != m_guiWantsKeyboard || mouse != m_guiWantsMouse) {
      queueInput({.type = REMIXAPI_INPUT_RESET});
    }
    m_guiWantsKeyboard = keyboard;
    m_guiWantsMouse = mouse;
  }

  namespace {
    uint32_t menuModifiers(SDL_Keymod mod) {
      return (mod & SDL_KMOD_ALT ? RemixSdlWindow::MenuAlt : 0) | (mod & SDL_KMOD_CTRL ? RemixSdlWindow::MenuCtrl : 0) |
             (mod & SDL_KMOD_SHIFT ? RemixSdlWindow::MenuShift : 0) | (mod & SDL_KMOD_GUI ? RemixSdlWindow::MenuGui : 0);
    }
  } // namespace

  void RemixSdlWindow::setMenuShortcut(remixapi_Key key, uint32_t modifiers) {
    const auto scancode = toScancode(key);
    if (scancode == SDL_SCANCODE_UNKNOWN)
      return;
    m_menuScancode = scancode;
    m_menuModifiers = modifiers & (MenuAlt | MenuCtrl | MenuShift | MenuGui);
  }

  bool RemixSdlWindow::takeCapturedMenuShortcut(remixapi_Key &key, uint32_t &modifiers) {
    if (!m_menuCaptured)
      return false;
    m_menuCaptured = false;
    key = m_capturedKey;
    modifiers = m_capturedModifiers;
    return true;
  }

  void RemixSdlWindow::setSceneCaptureShortcut(remixapi_Key key, uint32_t modifiers) {
    m_sceneScancode = key ? toScancode(key) : SDL_SCANCODE_UNKNOWN;
    m_sceneModifiers = modifiers & (MenuAlt | MenuCtrl | MenuShift | MenuGui);
  }

  bool RemixSdlWindow::takeCapturedSceneCaptureShortcut(remixapi_Key &key, uint32_t &modifiers) {
    if (!m_sceneCaptured)
      return false;
    m_sceneCaptured = false;
    key = m_capturedKey;
    modifiers = m_capturedModifiers;
    return true;
  }

  bool RemixSdlWindow::takeMenuToggle() {
    const bool toggle = (m_menuToggles & 1) != 0;
    m_menuToggles = 0;
    return toggle;
  }

  std::string RemixSdlWindow::shortcutName(remixapi_Key key, uint32_t modifiers) {
    std::string name;
#if defined(__APPLE__)
    const char *gui = "Cmd+";
#elif defined(_WIN32)
    const char *gui = "Win+";
#else
    const char *gui = "Super+";
#endif
    if (modifiers & MenuCtrl)
      name += "Ctrl+";
    if (modifiers & MenuAlt)
      name += "Alt+";
    if (modifiers & MenuShift)
      name += "Shift+";
    if (modifiers & MenuGui)
      name += gui;
    const char *keyName = SDL_GetScancodeName(toScancode(key));
    return name + (keyName && *keyName ? keyName : "?");
  }

  bool RemixSdlWindow::menuShortcut(const SDL_Event &event) {
    if (SDL_GetWindowFromEvent(&event) != m_window)
      return false;
    if (event.type == SDL_EVENT_TEXT_INPUT) {
      // macOS types a character for Option+key (Option+X: an approximately-equal sign).
      const bool swallow = m_swallowText;
      m_swallowText = false;
      return swallow;
    }
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
      return false;
    const auto scancode = event.key.scancode;
    if (event.type == SDL_EVENT_KEY_UP) {
      if (scancode != m_swallowedKey)
        return false;
      m_swallowedKey = SDL_SCANCODE_UNKNOWN;
      return true;
    }
    if (event.key.repeat)
      return scancode == m_swallowedKey;
    m_swallowText = false; // A key's text follows its press.
    const bool modifier = scancode >= SDL_SCANCODE_LCTRL && scancode <= SDL_SCANCODE_RGUI;
    if (m_capturingMenu && !modifier) {
      m_capturingMenu = false;
      const auto key = fromScancode(scancode);
      if (scancode != SDL_SCANCODE_ESCAPE && key) {
        m_capturedKey = key;
        m_capturedModifiers = menuModifiers(event.key.mod);
        (m_capturingScene ? m_sceneCaptured : m_menuCaptured) = true;
      }
      m_capturingScene = false;
    } else if (scancode == m_menuScancode && menuModifiers(event.key.mod) == m_menuModifiers) {
      ++m_menuToggles;
    } else if (m_sceneScancode != SDL_SCANCODE_UNKNOWN && scancode == m_sceneScancode &&
               menuModifiers(event.key.mod) == m_sceneModifiers) {
      ++m_scenePresses;
    } else {
      return false;
    }
    m_swallowedKey = scancode;
    m_swallowText = true;
    return true;
  }

  remixapi_Key RemixSdlWindow::fromScancode(SDL_Scancode scancode) {
    for (uint32_t key = REMIXAPI_KEY_A; key <= REMIXAPI_KEY_PAGE_DOWN; ++key) {
      if (toScancode(static_cast<remixapi_Key>(key)) == scancode)
        return static_cast<remixapi_Key>(key);
    }
    return remixapi_Key{};
  }

  void RemixSdlWindow::queueInput(const remixapi_InputEvent &event) {
    if (m_inputEvents.size() >= 4096) {
      m_inputEvents.clear();
      m_inputEvents.push_back({.type = REMIXAPI_INPUT_RESET});
    }
    m_inputEvents.push_back(event);
  }

  void RemixSdlWindow::processInput(const SDL_Event &event) {
    if (SDL_GetWindowFromEvent(&event) != m_window) {
      return;
    }
    remixapi_InputEvent input = {};
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      input.type = REMIXAPI_INPUT_RESET;
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      input.type = REMIXAPI_INPUT_KEY;
      input.code = fromScancode(event.key.scancode);
      if (!input.code)
        return;
      input.down = event.key.down;
      input.repeat = event.key.repeat;
      break;
    case SDL_EVENT_TEXT_INPUT: {
      if (!m_gameWantsText || !event.text.text)
        return;
      input.type = REMIXAPI_INPUT_TEXT;
      const char *text = event.text.text;
      while (*text) {
        input.code = SDL_StepUTF8(&text, nullptr);
        queueInput(input);
      }
      return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      input.type = REMIXAPI_INPUT_MOUSE_BUTTON;
      if (event.button.button < 1 || event.button.button > 5)
        return;
      input.code = toMouseButtons(SDL_BUTTON_MASK(event.button.button));
      input.down = event.button.down;
      input.x = event.button.x;
      input.y = event.button.y;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      input.type = REMIXAPI_INPUT_MOUSE_MOTION;
      input.x = event.motion.x;
      input.y = event.motion.y;
      input.dx = event.motion.xrel;
      input.dy = event.motion.yrel;
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      input.type = REMIXAPI_INPUT_MOUSE_WHEEL;
      input.x = event.wheel.x;
      input.y = event.wheel.y;
      if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        input.x = -input.x;
        input.y = -input.y;
      }
      break;
    default:
      return;
    }
    queueInput(input);
  }

  remixapi_ErrorCode RemixSdlWindow::pollInputEvents(remixapi_InputEvent *events, uint32_t capacity,
                                                     uint32_t &count) {
    count = 0;
    const auto status = checkThread();
    if (status != REMIXAPI_ERROR_CODE_SUCCESS)
      return status;
    if (capacity && !events)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (!capacity) {
      count = static_cast<uint32_t>(m_inputEvents.size());
    } else {
      while (count < capacity && !m_inputEvents.empty()) {
        events[count++] = m_inputEvents.front();
        m_inputEvents.pop_front();
      }
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

} // namespace remix
