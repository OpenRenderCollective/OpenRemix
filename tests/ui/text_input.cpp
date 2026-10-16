#include <cstdlib>
#include <cstring>
#include <iostream>

#include <SDL3/SDL.h>

#include <imgui_impl_sdl3.h>

namespace {
  void require(bool condition, const char* pMessage) {
    if (!condition) {
      std::cerr << pMessage << '\n';
      std::exit(1);
    }
  }

  void frame(char* pText, bool focus = false, bool showField = true) {
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Text input");
    if (showField) {
      if (focus) {
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::InputText("Value", pText, 256);
    }
    ImGui::End();
    ImGui::Render();
  }

  void textEvent(SDL_Window* pWindow, const char* pText) {
    SDL_Event event {};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = SDL_GetWindowID(pWindow);
    event.text.text = pText;
    require(ImGui_ImplSDL3_ProcessEvent(&event), "Text event was not accepted");
  }

  void keyEvent(SDL_Window* pWindow, SDL_Scancode key, bool down) {
    SDL_Event event {};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = SDL_GetWindowID(pWindow);
    event.key.scancode = key;
    event.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
    require(ImGui_ImplSDL3_ProcessEvent(&event), "Editing key was not accepted");
  }
}

int main() {
  // Headless, in code: Wine does not pass SDL_VIDEODRIVER to the process, and
  // a real window's global mouse state replaces the test's events.
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::cerr << "SDL video initialization failed: " << SDL_GetError() << '\n';
    return 1;
  }
  SDL_Window* pWindow = SDL_CreateWindow("Text input test", 640, 480, 0);
  require(pWindow != nullptr, "SDL window creation failed");
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().ConfigInputTrickleEventQueue = false;
  unsigned char* pPixels;
  int width, height;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pPixels, &width, &height);
  require(ImGui_ImplSDL3_InitForVulkan(pWindow), "SDL ImGui initialization failed");
  require(!SDL_TextInputActive(pWindow), "Text input started before a field was active");
  char text[256] {};
  frame(text, true);
  frame(text);
  frame(text);
  require(SDL_TextInputActive(pWindow), "Focused text field did not enable SDL text input");
  require(ImGui::GetIO().WantCaptureKeyboard, "Text field did not capture keyboard input");

  textEvent(pWindow, "file.name, -1.25 /[];:'\"\\|<>?~!@#$%^&*()_+\xc3\xa9");
  frame(text);
  require(std::strcmp(text, "file.name, -1.25 /[];:'\"\\|<>?~!@#$%^&*()_+\xc3\xa9") == 0,
    "Punctuation or Unicode text was lost");
  keyEvent(pWindow, SDL_SCANCODE_BACKSPACE, true);
  frame(text);
  keyEvent(pWindow, SDL_SCANCODE_BACKSPACE, false);
  frame(text);
  require(std::strcmp(text, "file.name, -1.25 /[];:'\"\\|<>?~!@#$%^&*()_+") == 0,
    "Backspace did not remove one Unicode character");

  SDL_Event foreign {};
  foreign.type = SDL_EVENT_TEXT_INPUT;
  foreign.text.windowID = SDL_GetWindowID(pWindow) + 1;
  foreign.text.text = "wrong window";
  require(!ImGui_ImplSDL3_ProcessEvent(&foreign), "Text from another window was accepted");
  frame(text);
  require(std::strstr(text, "wrong window") == nullptr, "Foreign text leaked into the field");
  frame(text, false, false);
  require(!SDL_TextInputActive(pWindow), "Leaving the field did not stop SDL text input");
  frame(text, true);
  frame(text);
  require(SDL_TextInputActive(pWindow), "Text input did not reactivate");
  ImGui_ImplSDL3_Shutdown();
  // The upstream backend leaves text input ownership to the application.
  SDL_StopTextInput(pWindow);
  require(!SDL_TextInputActive(pWindow), "Backend shutdown left text input active");
  ImGui::DestroyContext();
  SDL_DestroyWindow(pWindow);
  SDL_Quit();
  return 0;
}
