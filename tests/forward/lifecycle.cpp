#include "remix-native/scene.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <remix/remix_bake_ext.h>
#include <thread>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void checked(remixapi_ErrorCode code) {
    require(code == REMIXAPI_ERROR_CODE_SUCCESS, "Forward+ lifecycle API failure");
  }
} // namespace

int main() {
  std::fprintf(stderr, "Forward+ lifecycle: initialize library\n");
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  remixapi_Interface api{};
  checked(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = startup.windowHeight = 96;
  startup.windowTitle = "Forward+ lifecycle";
#endif
  std::fprintf(stderr, "Forward+ lifecycle: startup\n");
  checked(api.Startup(&startup));
  // Target replacement checks observe each completed frame. Exercise async
  // publication explicitly below rather than inheriting the host's defaults.
  checked(api.SetConfigVariable("rtx.native.asyncPresent", "0"));
  checked(api.SetConfigVariable("rtx.native.lowLatency", "0"));
  remixapi_BakeSceneInfoEXT loading{};
  loading.structSize = sizeof(loading);
  loading.version = REMIXAPI_BAKE_SCENE_VERSION_EXT;
  loading.sceneId = 1;
  loading.revision = 1;
  loading.sceneScale = .0254f;
  checked(remixapi_SetBakeSceneEXT(&loading));
  checked(remixapi_SetBakeSceneEXT(nullptr));
  int windowCount = 0;
  SDL_Window **windows = SDL_GetWindows(&windowCount);
  require(windowCount > 0 && windows, "Lifecycle window");
  SDL_SetWindowSize(windows[0], 96, 96);
  SDL_free(windows);
  checked(api.SetConfigVariable("rtx.native.forward.shadowResolution", "128"));
  checked(api.SetConfigVariable("rtx.native.forward.reflectionProbes", "0"));
  checked(api.SetConfigVariable("rtx.native.forward.samples", "4"));
  checked(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "off"));
  checked(api.SetConfigVariable("rtx.native.pathtrace.denoiser", "off"));
  checked(api.SetConfigVariable("rtx.native.pathtrace.taa", "0"));
  checked(api.SetConfigVariable("rtx.native.pathtrace.sharc", "0"));
  SampleScene scene;
  checked(scene.create(api));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  checked(remixapi_GetBackendCapabilitiesEXT(&caps));
  require(caps.supportedBackends & 4u, "Forward+ capability is independent of ray queries");
  const bool traceSupported = caps.supportedBackends & 2u;
  uint32_t drawWidth = 96, drawHeight = 96;
  auto waitBackend = [&](const char *name, uint32_t expected) {
    std::fprintf(stderr, "Forward+ lifecycle: request %s\n", name);
    checked(api.SetConfigVariable("rtx.native.backend", name));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    do {
      checked(scene.draw(api, 0, drawWidth, drawHeight));
      checked(api.Present(nullptr));
      checked(remixapi_GetBackendCapabilitiesEXT(&caps));
      if (caps.activeBackend == expected && caps.requestedBackend == expected)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    require(false, "Backend activation timed out");
  };
  waitBackend("forwardplus", 2);
  remixapi_BakeStatisticsEXT baking{};
  baking.structSize = sizeof(baking);
  checked(remixapi_GetBakeStatisticsEXT(&baking));
  require(baking.structSize == sizeof(baking) && !baking.active,
          "Bake statistics are queryable before a complete map is published");
  baking.structSize = 4;
  require(remixapi_GetBakeStatisticsEXT(&baking) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Bake statistics reject undersized output headers");
  require(remixapi_GetBakeStatisticsEXT(nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Bake statistics reject null outputs");
  baking.structSize = sizeof(baking);
  checked(remixapi_GetBakeStatisticsEXT(&baking));
  bool expectPublishedBake = false;
  uint64_t publishedHash = 0;
  remixapi_BakeSceneBoundsInfoEXT map{};
  if (baking.supported) {
    checked(api.SetConfigVariable("rtx.native.forward.bakeProbes", "64"));
    checked(api.SetConfigVariable("rtx.native.forward.bakeSamples", "64"));
    checked(api.SetConfigVariable("rtx.native.forward.bakeBounces", "1"));
    map.scene = {sizeof(map), REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT,
                 2,           1,
                 1,           REMIXAPI_BAKE_SCENE_READY_EXT,
                 0,           1,
                 nullptr,     &scene.daylight};
    for (uint32_t c = 0; c < 3; ++c) {
      map.minimum[c] = -2;
      map.maximum[c] = 2;
    }
    checked(remixapi_SetBakeSceneEXT(&map.scene));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
      checked(scene.draw(api, 0, drawWidth, drawHeight));
      checked(api.Present(nullptr));
      checked(remixapi_GetBakeStatisticsEXT(&baking));
      require(baking.state != REMIXAPI_BAKE_FAILED_EXT, "Lifecycle map bake succeeds");
      if (baking.active)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!baking.active)
      std::fprintf(stderr, "Lifecycle bake timeout: state %u, progress %u/%u; %s\n",
                   baking.state, baking.completed, baking.probes, baking.status);
    require(baking.active, "Lifecycle map bake publishes before target changes");
    publishedHash = baking.activeHash;
    expectPublishedBake = true;
  }
  checked(api.SetConfigVariable("rtx.native.vramStatistics", "1"));
  auto waitTargets = [&](auto ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    do {
      checked(scene.draw(api, 0, drawWidth, drawHeight));
      checked(api.Present(nullptr));
      checked(remixapi_GetBackendCapabilitiesEXT(&caps));
      if (expectPublishedBake && caps.activeBackend == 2) {
        checked(remixapi_GetBakeStatisticsEXT(&baking));
        if (!baking.active || baking.activeHash != publishedHash || baking.state != REMIXAPI_BAKE_READY_EXT) {
          std::fprintf(
              stderr, "Bake continuity at %ux%u: backend %u, state %u, active %u, hash %llu/%llu; %s\n",
              drawWidth, drawHeight, caps.activeBackend, baking.state, baking.active,
              (unsigned long long)baking.activeHash, (unsigned long long)publishedHash, baking.status);
        }
        require(baking.active && baking.activeHash == publishedHash &&
                    baking.state == REMIXAPI_BAKE_READY_EXT,
                "An active replacement target reuses the published map bake without retracing");
      }
      remixapi_VramStats memory{};
      checked(api.GetVramStats(&memory));
      if (caps.activeBackend == 2 && ready(memory.usedRenderTargetBytes))
        return memory.usedRenderTargetBytes;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    require(false, "Reflection target replacement timed out");
    return uint64_t(0);
  };
  const auto reflectedBytes = waitTargets([](auto bytes) { return bytes > 0; });
  checked(api.SetConfigVariable("rtx.native.forward.samples", "1"));
  const auto singleSampleBytes = waitTargets([&](auto bytes) { return bytes < reflectedBytes; });
  checked(api.SetConfigVariable("rtx.native.forward.samples", "4"));
  waitTargets([&](auto bytes) { return bytes > singleSampleBytes; });
  windows = SDL_GetWindows(&windowCount);
  require(windowCount > 0 && windows, "Lifecycle resize window");
  SDL_SetWindowSize(windows[0], 192, 160);
  SDL_free(windows);
  drawWidth = 192;
  drawHeight = 160;
  const auto resizedBytes = waitTargets([&](auto bytes) { return bytes > reflectedBytes; });
  windows = SDL_GetWindows(&windowCount);
  require(windowCount > 0 && windows, "Lifecycle restored window");
  SDL_SetWindowSize(windows[0], 96, 96);
  SDL_free(windows);
  drawWidth = drawHeight = 96;
  waitTargets([&](auto bytes) { return bytes < resizedBytes; });
  checked(api.SetConfigVariable("rtx.native.forward.ssr", "0"));
  const auto opaqueBytes = waitTargets([&](auto bytes) { return bytes < reflectedBytes; });
  checked(api.SetConfigVariable("rtx.native.forward.rtReflections", "1"));
  waitTargets([&](auto bytes) { return bytes > opaqueBytes; });
  expectPublishedBake = false;
  checked(remixapi_SetBakeSceneEXT(nullptr));
  for (uint32_t i = 0; i < 6; ++i) {
    checked(scene.draw(api, 0, drawWidth, drawHeight));
    checked(api.Present(nullptr));
    checked(remixapi_GetBakeStatisticsEXT(&baking));
    require(!baking.active && baking.activeHash == 0,
            "Map unload clears a bake retained across target replacements");
  }
  for (int i = 0; i < 6; ++i) {
    checked(scene.draw(api, 0, 96, 96));
    checked(api.Present(nullptr));
  }
  checked(api.SetConfigVariable("rtx.native.forward.rtReflections", "0"));
  waitTargets([&](auto bytes) { return bytes <= opaqueBytes; });
  // GI can change pipelines without changing attachment sizes. Exercise a
  // superseded preparation; native pixel tests separately verify active GI.
  checked(api.SetConfigVariable("rtx.native.forward.rtGI", "1"));
  for (int i = 0; i < 4; ++i) {
    checked(scene.draw(api, 0, 96, 96));
    checked(api.Present(nullptr));
  }
  checked(api.SetConfigVariable("rtx.native.forward.rtGI", "0"));
  // Let each request start preparing before replacing it. Obsolete workers must
  // retain their resources until completion without activating an old target.
  checked(api.SetConfigVariable("rtx.native.forward.ssr", "1"));
  for (const char *scale : {"4", "1", "2"}) {
    checked(api.SetConfigVariable("rtx.native.forward.ssrScale", scale));
    checked(scene.draw(api, 0, 96, 96));
    checked(api.Present(nullptr));
  }
  waitTargets([&](auto bytes) { return bytes > opaqueBytes; });
  waitBackend("raster", 0);
  waitBackend("forwardplus", 2);
  if (traceSupported) {
    waitBackend("pathtrace", 1);
    waitBackend("forwardplus", 2);
    waitBackend("pathtrace", 1);
    waitBackend("forwardplus", 2);
  }
  // Exercise frame publication and resource ownership after a backend switch.
  checked(api.SetConfigVariable("rtx.native.asyncPresent", "1"));
  for (int i = 0; i < 8; ++i) {
    checked(scene.draw(api, 0, 96, 96));
    checked(api.Present(nullptr));
  }
  checked(api.SetConfigVariable("rtx.native.asyncPresent", "0"));
  waitBackend("raster", 0);
  checked(api.Shutdown());
  std::puts("Forward+ runtime activation, backend round trips, asynchronous publication and shutdown passed");
}
