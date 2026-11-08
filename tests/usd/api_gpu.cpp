#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <remix/remix_c.h>
#include <remix/remix_editor_ext.h>
#include <remix/remix_usd_ext.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  remixapi_Interface api{};
  std::filesystem::path directory;
  try {
    require(argc == 2, "fixture path");
    remixapi_InitializeLibraryInfo initialize{};
    initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
    require(!remixapi_InitializeLibrary(&initialize, &api), "initialize API");
    directory = std::filesystem::temp_directory_path() / ("openremix-usd-api-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    auto path = directory / "capture.usda";
    std::filesystem::copy_file(argv[1], path);
    remixapi_EditorStartupEXT editor{REMIXAPI_STRUCT_TYPE_EDITOR_STARTUP_EXT, nullptr, 1, false, true, true};
    remixapi_StartupInfo startup{};
    startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    startup.pNext = &editor;
    startup.windowTitle = "USD reload regression";
    startup.windowWidth = startup.windowHeight = 64;
    require(!api.Startup(&startup), "startup");
    require(!api.SetConfigVariable("rtx.native.backend", "raster"), "raster backend");
    remixapi_USDLoadOptionsEXT options{sizeof(options), 1, path.c_str(), 0, 0, 0};
    remixapi_USDSceneHandleEXT first{}, second{};
    require(!remixapi_LoadUSDSceneEXT(&options, &first) && !remixapi_LoadUSDSceneEXT(&options, &second),
            "load two scenes");
    remixapi_USDDrawOptionsEXT draw{sizeof(draw), 1, false, 1};
    auto frame = [&] {
      require(!remixapi_DrawUSDSceneEXT(first, &draw) && !remixapi_DrawUSDSceneEXT(second, &draw), "draw two scenes");
      require(!api.Present(nullptr), "present");
    };
    frame();
    require(!remixapi_ReloadUSDSceneEXT(first) && !remixapi_ReloadUSDSceneEXT(second), "reload two scenes");
    remixapi_USDDiagnosticsEXT a{}, b{};
    a.structSize = sizeof(a);
    b.structSize = sizeof(b);
    for (int i = 0; i < 100; ++i) {
      frame();
      remixapi_GetUSDDiagnosticsEXT(first, &a);
      remixapi_GetUSDDiagnosticsEXT(second, &b);
      if (a.generation == 2 && b.generation == 2)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(a.generation == 2 && b.generation == 2, "every scene reload publishes at the frame boundary");
    std::ofstream(path) << "malformed USD";
    require(!remixapi_ReloadUSDSceneEXT(second), "reload malformed edit");
    for (int i = 0; i < 100; ++i) {
      frame();
      remixapi_GetUSDDiagnosticsEXT(second, &b);
      if (*b.message)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(*b.message && b.generation == 2, "malformed reload keeps the working generation and reports failure");
    require(!remixapi_DestroyUSDSceneEXT(first) && !remixapi_DestroyUSDSceneEXT(second), "destroy scenes");
    require(!api.Shutdown(), "shutdown");
    std::filesystem::remove_all(directory);
    std::puts("USD frame-boundary reload passed");
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    if (api.Shutdown)
      api.Shutdown();
    return 1;
  }
}
