#include "ui/profiles.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <iostream>

using namespace remix;
static void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
int main(int argc, char **argv) {
  require(argc == 2, "Expected isolated config directory");
  config::Profiles profiles;
  profiles.startup(argv[1]);
  require(profiles.save(), "Save fixture");
  ui::ProfileUi ui;
  // Headless, in code: Wine does not pass SDL_VIDEODRIVER to the process, and
  // a real window's global mouse state replaces the test's events.
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
  require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
  SDL_Window *window = SDL_CreateWindow("Profile UI test", 640, 480, 0);
  require(window, SDL_GetError());
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigInputTrickleEventQueue = false;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  unsigned char *pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  require(ImGui_ImplSDL3_InitForVulkan(window), "Initialize SDL backend");
  ui::toolkitStyle();
  auto frame = [&] {
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(520, 480));
    ImGui::Begin("Profile test");
    ui::drawProfiles(profiles, ui);
    ui::drawProfileFooter(profiles, ui);
    ui::drawProfileDialogs(profiles, ui);
    ImGui::End();
    ImGui::Render();
  };
  auto key = [&](SDL_Scancode scancode, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.scancode = scancode;
    event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
    require(ImGui_ImplSDL3_ProcessEvent(&event), "Key rejected");
    frame();
  };
  auto text = [&](const char *value) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = SDL_GetWindowID(window);
    event.text.text = value;
    require(ImGui_ImplSDL3_ProcessEvent(&event), "Text rejected");
    frame();
  };
  frame();
  ui.nameDialog = ui.duplicate = ui.focusName = true;
  frame();
  frame();
  frame();
  require(io.WantTextInput && io.WantCaptureKeyboard && SDL_TextInputActive(window),
          "Profile name field did not capture text input");
  text("Night caf\xc3\xa9");
  key(SDL_SCANCODE_RETURN, true);
  key(SDL_SCANCODE_RETURN, false);
  require(profiles.active() == "Night caf\xc3\xa9" && !ui.nameDialog,
          "Enter did not duplicate with Unicode name");
  ui.nameDialog = ui.duplicate = ui.focusName = true;
  ui.name[0] = 0;
  frame();
  frame();
  text("Default");
  key(SDL_SCANCODE_RETURN, true);
  key(SDL_SCANCODE_RETURN, false);
  require(ui.nameDialog && profiles.hasError() && profiles.active() == "Night caf\xc3\xa9",
          "Duplicate collision did not keep dialog and report error");
  key(SDL_SCANCODE_ESCAPE, true);
  key(SDL_SCANCODE_ESCAPE, false);
  frame();
  frame();
  require(!ui.nameDialog && !io.WantTextInput, "Cancel retained name input capture");
  auto edited = profiles.current();
  edited.trace.samples = 8;
  profiles.edit(edited);
  ui.actions.request(profiles, ui::ProfileAction::Load, "Missing");
  frame();
  frame();
  require(io.WantCaptureKeyboard, "Unsaved modal did not capture keyboard");
  require(!ui.actions.resolve(profiles, ui::UnsavedChoice::Discard) && profiles.hasError(),
          "Load failure lost feedback");
  frame();
  key(SDL_SCANCODE_ESCAPE, true);
  key(SDL_SCANCODE_ESCAPE, false);
  require(ui.actions.pending == ui::ProfileAction::None && profiles.current() == edited,
          "Escape from unsaved modal discarded edits");
  io.AddMousePosEvent(40, 40);
  frame();
  frame();
  require(io.WantCaptureMouse, "Panel did not capture mouse");
  for (float scale : {1.f, 2.f}) {
    for (ImVec2 size : {ImVec2(640, 480), ImVec2(320, 240)}) {
      for (bool right : {false, true}) {
        ImGui::GetStyle() = ImGuiStyle{};
        ui::toolkitStyle();
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleDpi = scale;
        profiles.savePanel({right, 300, 3});
        ui.restoreTab = true;
        int drawnTab = -1;
        for (int i = 0; i < 3; ++i) {
          ImGui_ImplSDL3_NewFrame();
          io.DisplaySize = size;
          ImGui::NewFrame();
          ui::drawPanel(profiles, ui, &drawnTab, [](void *user, int tab, config::Settings &) {
            *static_cast<int *>(user) = tab;
            for (int row = 0; row < 80; ++row)
              ImGui::Text("Scrollable row %d", row);
          });
          ImGui::Render();
        }
        require(drawnTab == 3 && profiles.panel().tab == 3, "Selected tab did not restore");
        auto *panel = ImGui::FindWindowByName("OpenRemix");
        require(panel && std::abs(panel->Size.y - size.y) < 1, "Panel did not fill viewport height");
        const float expectedX = right ? size.x - panel->Size.x : 0;
        require(std::abs(panel->Pos.x - expectedX) < 1, "Panel did not anchor to selected edge");
        bool footerVisible = false, scrollable = false;
        for (auto *child : ImGui::GetCurrentContext()->Windows) {
          if (!child->Active || child->ParentWindow != panel)
            continue;
          if (std::strstr(child->Name, "Profile footer"))
            footerVisible = child->Pos.y >= 0 && child->Pos.y + child->Size.y <= size.y &&
                            child->Size.y >= 2 * ImGui::GetFrameHeight() && child->ScrollMax.y < 1;
          if (std::strstr(child->Name, "Tab contents"))
            scrollable |= child->ScrollMax.y > 0;
        }
        require(footerVisible && scrollable, "Small/high-DPI panel clipped footer or lost scrolling");
      }
    }
  }
  // Starting a drag uses the visible edge, even when the remembered width is
  // wider than the current viewport. Pointer motion before the press is ignored.
  ImGui::GetStyle() = ImGuiStyle{};
  ui::toolkitStyle();
  profiles.savePanel({false, 600, 0});
  ui.restoreTab = true;
  auto panelFrame = [&] {
    ImGui_ImplSDL3_NewFrame();
    io.DisplaySize = ImVec2(400, 480);
    ImGui::NewFrame();
    ui::drawPanel(profiles, ui, nullptr, [](void *, int, config::Settings &) {});
    ImGui::Render();
  };
  panelFrame();
  panelFrame();
  io.AddMousePosEvent(398, 200);
  io.AddMouseButtonEvent(0, true);
  panelFrame();
  require(std::abs(profiles.panel().width - 400) < 1, "Resize press jumped to hidden remembered width");
  io.AddMousePosEvent(350, 200);
  panelFrame();
  require(std::abs(profiles.panel().width - 352) < 1, "Panel edge did not follow horizontal drag");
  io.AddMouseButtonEvent(0, false);
  panelFrame();
  config::Profiles layoutRestored;
  layoutRestored.startup(argv[1]);
  require(std::abs(layoutRestored.panel().width - 352) < 1, "Released panel width was not persisted");
  ImGui_ImplSDL3_Shutdown();
  SDL_StopTextInput(window);
  ImGui::DestroyContext();
  SDL_DestroyWindow(window);
  SDL_Quit();
}
