// Panel sliders follow a drag: the developer panel (ui::drawPanel) with the
// Display tab's FSR sharpening slider, dragged with simulated mouse input; the
// profile's value must move with the pointer and stay where it is released.
// Covers a format with nothing after the conversion ("%.2f"), which ImGui's
// format trimming returns as is, and one with trailing text ("%.0f px").
#include "ui/profiles.h"
#include "ui/widgets.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>

using namespace remix;
static void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

namespace {
  struct Row {
    ImVec2 min, max;
    bool drawn = false;
  } sharpening, sized;
  float pixels = 100; // the second slider's value; not a profile setting

  void record(Row &row) {
    row.min = ImGui::GetItemRectMin();
    row.max = ImGui::GetItemRectMax();
    row.drawn = ImGui::IsItemVisible();
  }
  // The Display tab's Upscaling panel as ui/settings.cpp draws it with FSR.
  void drawTab(void *, int tab, config::Settings &settings) {
    if (tab != int(ui::Tab::Display))
      return;
    auto &options = settings.trace;
    if (ui::Panel panel{"Upscaling", ImGuiTreeNodeFlags_DefaultOpen}) {
      int upscaler = int(options.upscaler);
      if (ui::Combo("Requested upscaler", &upscaler, "Off\0Auto\0FSR\0DLSS\0XeSS\0"))
        options.upscaler = upscale::Mode(upscaler);
      ui::SliderFloat("FSR sharpening", &options.fsrSharpness, 0.f, 1.f, "%.2f");
      record(sharpening);
      ui::SliderFloat("Size", &pixels, 0.f, 200.f, "%.0f px");
      record(sized);
    }
  }
} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "Expected isolated config directory");
  config::Profiles profiles;
  profiles.startup(argv[1]);
  auto settings = profiles.current();
  settings.trace.upscaler = upscale::Mode::Fsr31;
  settings.trace.fsrSharpness = 0;
  require(profiles.edit(settings), "Fixture settings rejected");
  ui::ProfileUi ui;
  // Headless, in code, as tests/ui/profiles.cpp: a real window's global mouse
  // state would replace the test's events.
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
  require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
  SDL_Window *window = SDL_CreateWindow("Slider test", 900, 700, 0);
  require(window, SDL_GetError());
  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigInputTrickleEventQueue = false;
  unsigned char *fontPixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&fontPixels, &width, &height);
  require(ImGui_ImplSDL3_InitForVulkan(window), "Initialize SDL backend");
  ui::toolkitStyle();
  profiles.savePanel({false, 600, int(ui::Tab::Display)});
  ui.restoreTab = true;
  auto frame = [&] {
    ImGui_ImplSDL3_NewFrame();
    io.DisplaySize = ImVec2(900, 700);
    ImGui::NewFrame();
    ui::drawPanel(profiles, ui, nullptr, drawTab);
    ImGui::Render();
  };
  for (int i = 0; i < 4; ++i)
    frame();
  require(sharpening.drawn && sized.drawn, "Sliders not drawn");

  // Press near a slider's left end, drag right by 40% of its width in steps
  // (past the drag threshold), release.
  auto drag = [&](const Row &row) {
    const float y = (row.min.y + row.max.y) / 2, span = row.max.x - row.min.x;
    float x = row.min.x + 4;
    io.AddMousePosEvent(x, y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    for (int step = 0; step < 8; ++step) {
      x += span * .05f;
      io.AddMousePosEvent(x, y);
      frame();
    }
    io.AddMouseButtonEvent(0, false);
    frame();
    frame();
  };
  drag(sharpening);
  const float sharpness = profiles.current().trace.fsrSharpness;
  std::fprintf(stderr, "FSR sharpening after the drag: %.3f\n", sharpness);
  require(sharpness > .2f && sharpness < .6f, "FSR sharpening slider (\"%.2f\") did not follow the drag");
  require(std::abs(sharpness * 100 - std::round(sharpness * 100)) < 1e-3f,
          "FSR sharpening was not rounded to its format's two decimals");
  pixels = 0;
  drag(sized);
  std::fprintf(stderr, "Size after the drag: %.3f\n", pixels);
  require(pixels > 40 && pixels < 120 && pixels == std::round(pixels),
          "Slider with trailing text (\"%.0f px\") did not follow the drag");

  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyWindow(window);
  SDL_Quit();
}
