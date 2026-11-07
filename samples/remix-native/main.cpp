#include "scene.h"
#include <remix/remix_c.h>
#include <remix/remix_usd_ext.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <filesystem>
#if defined(_WIN32)
#include <string>
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  std::wstring path = L"remix.dll";
  if (argc > 1) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    if (!count) {
      return 1;
    }
    path.resize(count);
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path.data(), count);
  }
  const auto libraryPath = path.c_str();
#elif defined(__APPLE__)
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.0.dylib";
#elif defined(__ANDROID__)
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.so";
#else
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.so.0";
#endif
  const unsigned maxFrames = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 0;
  remixapi_Interface api = {};
  remixapi_HMODULE library = nullptr;
  auto status = remixapi_lib_loadRemixDllAndInitialize(libraryPath, &api, &library);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "Remix load failed: 0x%x\n", unsigned(status));
    return 1;
  }
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowTitle = "Remix Raster";
#endif
  status = api.Startup(&startup);
  if (status == REMIXAPI_ERROR_CODE_SUCCESS && argc > 3) {
    auto selected = api.SetConfigVariable("rtx.native.backend", argv[3]);
    if (selected != REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE)
      status = selected;
  }
  remixapi_USDSceneHandleEXT usdScene = 0;
  using LoadUSD = remixapi_ErrorCode(REMIXAPI_CALL *)(const remixapi_USDLoadOptionsEXT *, remixapi_USDSceneHandleEXT *);
  using DrawUSD = remixapi_ErrorCode(REMIXAPI_CALL *)(remixapi_USDSceneHandleEXT, const remixapi_USDDrawOptionsEXT *);
  using DestroyUSD = remixapi_ErrorCode(REMIXAPI_CALL *)(remixapi_USDSceneHandleEXT);
#if defined(_WIN32)
  auto symbol = [&](const char *name) { return GetProcAddress(library, name); };
#else
  auto symbol = [&](const char *name) { return dlsym(library, name); };
#endif
  auto loadUSD = reinterpret_cast<LoadUSD>(symbol("remixapi_LoadUSDSceneEXT"));
  auto drawUSD = reinterpret_cast<DrawUSD>(symbol("remixapi_DrawUSDSceneEXT"));
  auto destroyUSD = reinterpret_cast<DestroyUSD>(symbol("remixapi_DestroyUSDSceneEXT"));
  if (status == REMIXAPI_ERROR_CODE_SUCCESS && argc > 4) {
    remixapi_USDLoadOptionsEXT options{sizeof(options), 1, argv[4], 0, 0, 0};
    status = loadUSD && drawUSD && destroyUSD ? loadUSD(&options, &usdScene) : REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    using Diagnostic = remixapi_ErrorCode(REMIXAPI_CALL *)(remixapi_USDSceneHandleEXT, remixapi_USDDiagnosticsEXT *);
    using ConfigureMods = remixapi_ErrorCode(REMIXAPI_CALL *)(const remixapi_USDModsOptionsEXT *);
    auto diagnostics = reinterpret_cast<Diagnostic>(symbol("remixapi_GetUSDDiagnosticsEXT"));
    auto configureMods = reinterpret_cast<ConfigureMods>(symbol("remixapi_ConfigureUSDModsEXT"));
    if (status == REMIXAPI_ERROR_CODE_SUCCESS && diagnostics && configureMods) {
      remixapi_USDDiagnosticsEXT d{};
      d.structSize = sizeof(d);
      status = diagnostics(usdScene, &d);
      const auto directory = std::getenv("OPENREMIX_MODS_DIRECTORY");
      const auto defaultDirectory = (std::filesystem::current_path() / "rtx-remix/mods").string();
      remixapi_USDModsOptionsEXT mods{sizeof(mods),    1,        directory ? directory : defaultDirectory.c_str(),
                                      d.metersPerUnit, d.upAxis, true};
      if (status == REMIXAPI_ERROR_CODE_SUCCESS)
        status = configureMods(&mods);
      std::printf("USD capture: %llu meshes, %llu materials, %llu lights; %.9g meters/unit, %s up\n",
                  (unsigned long long)d.meshes, (unsigned long long)d.materials, (unsigned long long)d.lights,
                  d.metersPerUnit, d.upAxis == 1 ? "Y" : "Z");
    }
    // Standalone capture replay uses authored lighting, not a game profile sun.
    api.SetConfigVariable("rtx.native.pathtrace.overrideLight", "0");
    api.SetConfigVariable("rtx.native.pathtrace.localRange", "0");
  }
  SampleScene scene;
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    if (!usdScene)
      status = scene.create(api);
  }
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    status = api.SetUIState(REMIXAPI_UI_STATE_ADVANCED);
  }
  const auto start = std::chrono::steady_clock::now();
  for (unsigned frame = 0; status == REMIXAPI_ERROR_CODE_SUCCESS && (!maxFrames || frame < maxFrames); ++frame) {
    status = api.PumpEvents();
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
      break;
    }
    remixapi_WindowState state = {};
    status = api.GetWindowState(&state);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS || state.closeRequested) {
      break;
    }
    if (usdScene) {
      remixapi_USDDrawOptionsEXT options{sizeof(options), 1, true,
                                         float(state.drawableWidth) /
                                             float(state.drawableHeight ? state.drawableHeight : 1)};
      status = drawUSD(usdScene, &options);
    } else
      status = scene.draw(api, std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count(),
                          state.drawableWidth, state.drawableHeight);
    if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
      status = api.Present(nullptr);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(state.drawableWidth ? 1 : 50));
  }
  if (usdScene)
    destroyUSD(usdScene);
  const auto shutdown = remixapi_lib_shutdownAndUnloadRemixDll(&api, library);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS || shutdown != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "Remix runtime failed: 0x%x, shutdown: 0x%x\n", unsigned(status), unsigned(shutdown));
    return 1;
  }
  return 0;
}
