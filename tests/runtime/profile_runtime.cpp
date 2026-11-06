#include "remix-native/scene.h"
#include "runtime/profiles.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <remix/remix_native_ext.h>
#include <remix/remix_reconstruction_ext.h>

using namespace remix;
namespace fs = std::filesystem;
static void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
static void checked(remixapi_ErrorCode code, int line) {
  if (code) {
    std::cerr << "API error " << unsigned(code) << " at line " << line << ": " << SDL_GetError() << '\n';
    std::exit(1);
  }
}
#define ok(code) checked((code), __LINE__)
int main(int argc, char **argv) {
  require(argc == 3, "Expected configuration and screenshot directories");
  config::Profiles profiles;
  profiles.startup(argv[1]);
  auto tracing = profiles.current();
  tracing.backend = 1;
  tracing.trace.upscaler = upscale::Mode::Off;
  tracing.trace.denoiser = false;
  tracing.trace.taa = false;
  tracing.trace.sharc = false;
  tracing.trace.samples = 3;
  tracing.clearColor = {.3f, .05f, .07f, 1};
  require(profiles.edit(tracing) && profiles.duplicate("A pathtrace"), "Path profile fixture");
  auto raster = config::Settings{};
  raster.backend = 0;
  raster.vsync = true;
  raster.clearColor = {.05f, .2f, .3f, 1};
  require(profiles.edit(raster) && profiles.duplicate("B raster"), "Raster profile fixture");
  auto reconstruction = tracing;
  reconstruction.trace.upscaler = upscale::Mode::Fsr31;
  reconstruction.trace.denoiser = true;
  reconstruction.trace.taa = true;
  require(profiles.edit(reconstruction) && profiles.duplicate("C reconstruction"),
          "Reconstruction profile fixture");
  require(profiles.load("A pathtrace"), "Select startup profile");
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  remixapi_Interface api{};
  ok(remixapi_InitializeLibrary(&initialize, &api));
  fs::create_directories(argv[2]);
  const bool uiOnly = std::getenv("REMIX_TEST_PROFILE_UI_ONLY") != nullptr;
  for (int cycle = 0; cycle < 3; ++cycle) {
    if (cycle)
      profiles.savePanel({cycle == 1, 300, cycle == 1 ? 4 : 2});
    remixapi_StartupInfo startup{};
    startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
    startup.windowWidth = cycle ? 320 : 640;
    startup.windowHeight = cycle ? 240 : 480;
#endif
    ok(api.Startup(&startup));
    require(api.GetUIState() == REMIXAPI_UI_STATE_NONE, "Startup restored menu visibility");
    remixapi_BackendCapabilitiesEXT caps{};
    caps.structSize = sizeof(caps);
    remixapi_DenoisingStatisticsEXT denoise{};
    denoise.structSize = sizeof(denoise);
    remixapi_ReconstructionDiagnosticsEXT upscale{};
    upscale.structSize = sizeof(upscale);
    ok(remixapi_GetBackendCapabilitiesEXT(&caps));
    ok(remixapi_GetDenoisingStatisticsEXT(&denoise));
    require(caps.requestedBackend == 1 && !denoise.requestedDenoiser && !denoise.requestedTaa,
            "Startup returned before restoring saved settings");
    // Launchers retain their explicit overrides after Startup.
    ok(api.SetConfigVariable("rtx.native.backend", "raster"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", "reblur-sh"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.taa", "true"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "fsr31"));
    ok(api.SetConfigVariable("rtx.native.clearColor.r", ".12"));
    require(api.SetConfigVariable("rtx.native.clearColor.r", "nan") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "Invalid clear color accepted through API");
    ok(api.SetConfigVariable("rtx.native.clearColor.unknown", "legacy"));
    ok(remixapi_GetBackendCapabilitiesEXT(&caps));
    ok(remixapi_GetDenoisingStatisticsEXT(&denoise));
    ok(remixapi_GetReconstructionDiagnosticsEXT(&upscale));
    require(caps.requestedBackend == 0 && denoise.requestedDenoiser && denoise.requestedTaa &&
                upscale.requestedMode == 2,
            "API did not override startup profile");
    SampleScene scene;
    ok(scene.create(api));
    auto frame = [&] {
      ok(api.PumpEvents());
      remixapi_WindowState state{};
      ok(api.GetWindowState(&state));
      ok(scene.draw(api, 0, state.drawableWidth, state.drawableHeight));
      ok(api.Present(nullptr));
    };
    for (int i = 0; i < 3; ++i)
      frame();
    ok(api.SetUIState(REMIXAPI_UI_STATE_ADVANCED));
    frame();
    frame();
    auto capture = [&](const char *name) {
      for (int i = 0; i < 20; ++i) {
        frame();
        SDL_Delay(16);
      }
      auto screenshot = fs::path(argv[2]) / name;
      ok(api.RequestPresentedScreenshot(screenshot.string().c_str()));
      frame();
      frame();
    };
    capture(cycle == 0 ? "rendering.png" : cycle == 1 ? "profiles-small.png" : "post-small.png");
    if (cycle == 0 && !uiOnly) {
      int windowCount = 0;
      auto windows = SDL_GetWindows(&windowCount);
      require(windowCount == 1, "Expected one runtime window");
      const auto id = SDL_GetWindowID(windows[0]);
      SDL_RaiseWindow(windows[0]);
      SDL_free(windows);
      auto key = [&](SDL_Scancode code) {
        for (bool down : {true, false}) {
          SDL_Event event{};
          event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
          event.key.windowID = id;
          event.key.scancode = code;
          event.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
          require(SDL_PushEvent(&event), "Push UI key");
          frame();
          frame();
        }
      };
      auto click = [&](float x, float y) {
        SDL_Event focus{};
        focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        focus.window.windowID = id;
        SDL_PushEvent(&focus);
        // ImGui's SDL3 backend takes the OS cursor's position while the
        // focused window is not hovered (macOS, Windows, X11): put it there.
        SDL_WarpMouseInWindow(SDL_GetWindowFromID(id), x, y);
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = id;
        motion.motion.x = x;
        motion.motion.y = y;
        require(SDL_PushEvent(&motion), "Push UI motion");
        frame();
        frame();
        for (bool down : {true, false}) {
          SDL_Event event{};
          event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
          event.button.windowID = id;
          event.button.button = SDL_BUTTON_LEFT;
          event.button.down = down;
          event.button.x = x;
          event.button.y = y;
          require(SDL_PushEvent(&event), "Push UI click");
          frame();
          frame();
        }
      };
      click(400, 36);
      capture("profiles.png");
      // The combo gives keyboard focus to the current profile. Move to B, then
      // discard the startup API overrides through the actual unsaved modal.
      click(100, 434);
      key(SDL_SCANCODE_DOWN);
      key(SDL_SCANCODE_RETURN);
      capture("unsaved.png");
      key(SDL_SCANCODE_RIGHT);
      key(SDL_SCANCODE_RETURN);
      frame();
      config::Profiles selected;
      selected.startup(argv[1]);
      require(selected.active() == "B raster", "UI load/discard did not switch profile");
      for (const auto &[keyCode, expected] : {std::pair{SDL_SCANCODE_DOWN, "C reconstruction"},
                                              {SDL_SCANCODE_UP, "B raster"},
                                              {SDL_SCANCODE_UP, "A pathtrace"}}) {
        click(100, 434);
        key(keyCode);
        key(SDL_SCANCODE_RETURN);
        frame();
        frame();
        selected.startup(argv[1]);
        if (selected.active() != expected) {
          capture("switch-failure.png");
          std::cerr << "Expected " << expected << ", selected " << selected.active() << '\n';
        }
        require(selected.active() == expected, "Live profile selector did not load requested profile");
        ok(remixapi_GetBackendCapabilitiesEXT(&caps));
        require(caps.requestedBackend == selected.current().backend,
                "Profile load did not update runtime request");
      }
      // Closing the menu retains live edits; it only releases UI capture.
      ok(api.SetUIState(REMIXAPI_UI_STATE_NONE));
    }
    if (cycle == 0 && !uiOnly) {
      // Exercise VSync, backend and reconstruction transitions on submitted frames.
      for (const char *mode : {"off", "fsr31", "auto", "off"}) {
        ok(api.SetConfigVariable("rtx.native.vsync", mode[0] == 'o' ? "false" : "true"));
        ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", mode));
        auto result = api.SetConfigVariable("rtx.native.backend", "pathtrace");
        require(result == REMIXAPI_ERROR_CODE_SUCCESS || result == REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE,
                "Backend request status changed");
        frame();
        frame();
        ok(api.SetConfigVariable("rtx.native.backend", "raster"));
        frame();
      }
    }
    ok(api.Shutdown());
  }
  // Shutdown never saves API/UI edits over the startup file.
  config::Profiles restored;
  restored.startup(argv[1]);
  require(restored.current() == tracing && restored.active() == "A pathtrace",
          "Exit silently saved live edits");
  // Start directly from a requested backend unsupported by the selected device.
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  ok(api.Startup(&startup));
  if (!uiOnly)
    ok(api.Present(nullptr));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  require(caps.requestedBackend == 1, "Backend fallback erased saved preference");
  if (!(caps.supportedBackends & 2))
    require(caps.activeBackend == 0 && caps.unavailableReason[0],
            "Unsupported backend did not fall back with explanation");
  ok(api.Shutdown());
  return 0;
}
