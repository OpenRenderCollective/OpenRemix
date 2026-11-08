#include "platform/window.h"
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
} // namespace

int main() {
  remix::RemixSdlWindow window;
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  require(window.initialize(startup) == REMIXAPI_ERROR_CODE_SUCCESS, "Window startup");
  require(window.pumpEvents() == REMIXAPI_ERROR_CODE_SUCCESS, "Initial events");
  std::array<remixapi_InputEvent, 4096> input;
  uint32_t count;
  window.pollInputEvents(input.data(), input.size(), count);
  window.setGuiCapture(true, true);
  window.setTextInputEnabled(true);
  window.pollInputEvents(input.data(), input.size(), count);

  SDL_Event event = {};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.windowID = SDL_GetWindowID(window.handle());
  event.key.scancode = SDL_SCANCODE_F12;
  event.key.down = true;
  require(SDL_PushEvent(&event), "Push UI shortcut");
  event = {};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.windowID = SDL_GetWindowID(window.handle());
  event.wheel.y = 1;
  require(SDL_PushEvent(&event), "Push wheel");
  event = {};
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.windowID = SDL_GetWindowID(window.handle());
  event.text.text = "x";
  require(SDL_PushEvent(&event), "Push captured text");
  require(window.pumpEvents() == REMIXAPI_ERROR_CODE_SUCCESS, "Captured events");
  window.pollInputEvents(input.data(), input.size(), count);
  require(count == 3 && input[0].type == REMIXAPI_INPUT_KEY && input[0].code == REMIXAPI_KEY_F12 &&
              input[1].type == REMIXAPI_INPUT_MOUSE_WHEEL && input[1].y == 1 && input[2].type == REMIXAPI_INPUT_TEXT &&
              input[2].code == 'x',
          "Raw captured input lost");
  remixapi_WindowState state = {};
  window.getWindowState(state);
  require(state.guiWantsKeyboard && state.guiWantsMouse, "Capture flags lost");
  remixapi_Bool pressed = true;
  window.isKeyDown(REMIXAPI_KEY_F12, pressed);
  require(!pressed, "Captured key leaked through legacy polling");
}
