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

  remixapi_ErrorCode RemixSdlWindow::pollMouseState(remixapi_MouseState &state) {
    remixapi_ErrorCode threadStatus = checkThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    float x = 0.0f;
    float y = 0.0f;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
    state.x = x;
    state.y = y;
    state.relativeX = m_relativeX;
    state.relativeY = m_relativeY;
    state.wheelX = m_wheelX;
    state.wheelY = m_wheelY;
    state.buttons = toMouseButtons(buttons);
    m_relativeX = 0.0f;
    m_relativeY = 0.0f;
    m_wheelX = 0.0f;
    m_wheelY = 0.0f;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixSdlWindow::isKeyDown(remixapi_Key key, remixapi_Bool &isDown) const {
    remixapi_ErrorCode threadStatus = checkThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    const SDL_Scancode scancode = toScancode(key);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    int keyCount = 0;
    const bool *keyboardState = SDL_GetKeyboardState(&keyCount);
    const int index = static_cast<int>(scancode);
    if (keyboardState == nullptr || index < 0 || index >= keyCount) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    isDown = keyboardState[index];
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixSdlWindow::setMouseGrabbed(bool grabbed) {
    remixapi_ErrorCode threadStatus = checkThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    if (!SDL_SetWindowMouseGrab(m_window, grabbed) || !SDL_SetWindowRelativeMouseMode(m_window, grabbed)) {
      setSdlError("SDL_SetWindowRelativeMouseMode");
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixSdlWindow::setCursorPosition(float x, float y) {
    remixapi_ErrorCode threadStatus = checkThread();
    if (threadStatus != REMIXAPI_ERROR_CODE_SUCCESS) {
      return threadStatus;
    }

    SDL_WarpMouseInWindow(m_window, x, y);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixSdlWindow::setTextInputEnabled(bool enabled) {
    const auto status = checkThread();
    if (status != REMIXAPI_ERROR_CODE_SUCCESS)
      return status;
    m_gameWantsText = enabled;
    syncTextInput(m_guiWantsText);
    const bool active = SDL_TextInputActive(m_window);
    // An embedded window may keep the host's own text input on.
    return active == (m_gameWantsText || m_guiWantsText) || (m_embedded && active)
               ? REMIXAPI_ERROR_CODE_SUCCESS
               : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  void RemixSdlWindow::syncTextInput(bool guiWantsText) {
    m_guiWantsText = guiWantsText;
    if (m_gameWantsText || m_guiWantsText) {
      if (!SDL_TextInputActive(m_window)) {
        SDL_StartTextInput(m_window);
        m_startedTextInput = true;
      }
    } else {
      // In a host's own window the host may have text input on for itself
      // (Xash3D's console): stopping it every frame left the console deaf.
      if (SDL_TextInputActive(m_window) && (!m_embedded || m_startedTextInput))
        SDL_StopTextInput(m_window);
      m_startedTextInput = false;
    }
  }

  SDL_Scancode RemixSdlWindow::toScancode(remixapi_Key key) {
    if (key >= REMIXAPI_KEY_A && key <= REMIXAPI_KEY_Z) {
      return static_cast<SDL_Scancode>(int(SDL_SCANCODE_A) + int(key) - int(REMIXAPI_KEY_A));
    }
    if (key >= REMIXAPI_KEY_1 && key <= REMIXAPI_KEY_9) {
      return static_cast<SDL_Scancode>(int(SDL_SCANCODE_1) + int(key) - int(REMIXAPI_KEY_1));
    }

    switch (key) {
    case REMIXAPI_KEY_0:
      return SDL_SCANCODE_0;
    case REMIXAPI_KEY_ESCAPE:
      return SDL_SCANCODE_ESCAPE;
    case REMIXAPI_KEY_ENTER:
      return SDL_SCANCODE_RETURN;
    case REMIXAPI_KEY_TAB:
      return SDL_SCANCODE_TAB;
    case REMIXAPI_KEY_BACKSPACE:
      return SDL_SCANCODE_BACKSPACE;
    case REMIXAPI_KEY_SPACE:
      return SDL_SCANCODE_SPACE;
    case REMIXAPI_KEY_LEFT:
      return SDL_SCANCODE_LEFT;
    case REMIXAPI_KEY_RIGHT:
      return SDL_SCANCODE_RIGHT;
    case REMIXAPI_KEY_UP:
      return SDL_SCANCODE_UP;
    case REMIXAPI_KEY_DOWN:
      return SDL_SCANCODE_DOWN;
    case REMIXAPI_KEY_LEFT_SHIFT:
      return SDL_SCANCODE_LSHIFT;
    case REMIXAPI_KEY_RIGHT_SHIFT:
      return SDL_SCANCODE_RSHIFT;
    case REMIXAPI_KEY_LEFT_CONTROL:
      return SDL_SCANCODE_LCTRL;
    case REMIXAPI_KEY_RIGHT_CONTROL:
      return SDL_SCANCODE_RCTRL;
    case REMIXAPI_KEY_LEFT_ALT:
      return SDL_SCANCODE_LALT;
    case REMIXAPI_KEY_RIGHT_ALT:
      return SDL_SCANCODE_RALT;
    case REMIXAPI_KEY_F1:
      return SDL_SCANCODE_F1;
    case REMIXAPI_KEY_F2:
      return SDL_SCANCODE_F2;
    case REMIXAPI_KEY_F3:
      return SDL_SCANCODE_F3;
    case REMIXAPI_KEY_F4:
      return SDL_SCANCODE_F4;
    case REMIXAPI_KEY_F5:
      return SDL_SCANCODE_F5;
    case REMIXAPI_KEY_F6:
      return SDL_SCANCODE_F6;
    case REMIXAPI_KEY_F7:
      return SDL_SCANCODE_F7;
    case REMIXAPI_KEY_F8:
      return SDL_SCANCODE_F8;
    case REMIXAPI_KEY_F9:
      return SDL_SCANCODE_F9;
    case REMIXAPI_KEY_F10:
      return SDL_SCANCODE_F10;
    case REMIXAPI_KEY_F11:
      return SDL_SCANCODE_F11;
    case REMIXAPI_KEY_F12:
      return SDL_SCANCODE_F12;
    case REMIXAPI_KEY_APOSTROPHE:
      return SDL_SCANCODE_APOSTROPHE;
    case REMIXAPI_KEY_COMMA:
      return SDL_SCANCODE_COMMA;
    case REMIXAPI_KEY_MINUS:
      return SDL_SCANCODE_MINUS;
    case REMIXAPI_KEY_PERIOD:
      return SDL_SCANCODE_PERIOD;
    case REMIXAPI_KEY_SLASH:
      return SDL_SCANCODE_SLASH;
    case REMIXAPI_KEY_SEMICOLON:
      return SDL_SCANCODE_SEMICOLON;
    case REMIXAPI_KEY_EQUAL:
      return SDL_SCANCODE_EQUALS;
    case REMIXAPI_KEY_LEFT_BRACKET:
      return SDL_SCANCODE_LEFTBRACKET;
    case REMIXAPI_KEY_BACKSLASH:
      return SDL_SCANCODE_BACKSLASH;
    case REMIXAPI_KEY_RIGHT_BRACKET:
      return SDL_SCANCODE_RIGHTBRACKET;
    case REMIXAPI_KEY_GRAVE_ACCENT:
      return SDL_SCANCODE_GRAVE;
    case REMIXAPI_KEY_INSERT:
      return SDL_SCANCODE_INSERT;
    case REMIXAPI_KEY_DELETE:
      return SDL_SCANCODE_DELETE;
    case REMIXAPI_KEY_HOME:
      return SDL_SCANCODE_HOME;
    case REMIXAPI_KEY_END:
      return SDL_SCANCODE_END;
    case REMIXAPI_KEY_PAGE_UP:
      return SDL_SCANCODE_PAGEUP;
    case REMIXAPI_KEY_PAGE_DOWN:
      return SDL_SCANCODE_PAGEDOWN;
    default:
      return SDL_SCANCODE_UNKNOWN;
    }
  }

  uint32_t RemixSdlWindow::toMouseButtons(SDL_MouseButtonFlags buttons) {
    uint32_t result = 0;
    if (buttons & SDL_BUTTON_LMASK) {
      result |= REMIXAPI_MOUSE_BUTTON_LEFT_BIT;
    }
    if (buttons & SDL_BUTTON_MMASK) {
      result |= REMIXAPI_MOUSE_BUTTON_MIDDLE_BIT;
    }
    if (buttons & SDL_BUTTON_RMASK) {
      result |= REMIXAPI_MOUSE_BUTTON_RIGHT_BIT;
    }
    if (buttons & SDL_BUTTON_X1MASK) {
      result |= REMIXAPI_MOUSE_BUTTON_X1_BIT;
    }
    if (buttons & SDL_BUTTON_X2MASK) {
      result |= REMIXAPI_MOUSE_BUTTON_X2_BIT;
    }
    return result;
  }

} // namespace remix
