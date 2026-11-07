#include "remix-native/scene.h"
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <condition_variable>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
  std::atomic<bool> holdCompile{false}, entered{false}, failCompile{false}, failImage{false};
  std::mutex gateMutex;
  std::condition_variable gate;
  void require(bool value, const char *message) {
    if (!value) {
      holdCompile = false;
      gate.notify_all();
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void ok(remixapi_ErrorCode code) { require(!code, "API call failed"); }
} // namespace
// Inject at the driver boundary; all requests still use the production worker,
// API mutex, queue submission, input processing and activation implementation.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache,
                                                                   uint32_t count,
                                                                   const VkComputePipelineCreateInfo *infos,
                                                                   const VkAllocationCallbacks *alloc,
                                                                   VkPipeline *out) {
  if (holdCompile) {
    entered = true;
    std::unique_lock lock(gateMutex);
    gate.wait(lock, [] { return !holdCompile.load(); });
  }
  if (failCompile.exchange(false)) {
    std::fill_n(out, count, VK_NULL_HANDLE);
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  static auto original = reinterpret_cast<PFN_vkCreateComputePipelines>(dlsym(RTLD_NEXT, "vkCreateComputePipelines"));
  return original(device, cache, count, infos, alloc, out);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
                                                        const VkAllocationCallbacks *alloc, VkImage *out) {
  if (failImage.exchange(false)) {
    *out = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  static auto original = reinterpret_cast<PFN_vkCreateImage>(dlsym(RTLD_NEXT, "vkCreateImage"));
  return original(device, info, alloc, out);
}
int main(int argc, char **argv) {
  const auto applicationStart = std::chrono::steady_clock::now();
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.windowWidth = 640;
  startup.windowHeight = 480;
  ok(api.Startup(&startup));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2)) {
    ok(api.Shutdown());
    return 77;
  }
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  ok(api.SetConfigVariable("rtx.native.vsync", "false"));
  config("upscaler", "off");
  config("denoiser", "off");
  config("taa", "false");
  config("sharc", "false");
  config("sigma", "false");
  config("bounces", "0");
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  SampleScene scene;
  ok(scene.create(api));
  auto &runtime = remix::api::g_runtime;
  auto frame = [&] {
    ok(api.PumpEvents());
    // The client owns the F12 binding and consumes the runtime's raw events.
    remixapi_InputEvent events[64]{};
    uint32_t count = 0;
    ok(runtime.window().pollInputEvents(events, 64, count));
    for (uint32_t i = 0; i < count; ++i)
      if (events[i].type == REMIXAPI_INPUT_KEY && events[i].code == REMIXAPI_KEY_F12)
        ok(api.SetUIState(REMIXAPI_UI_STATE_ADVANCED));
    remixapi_WindowState window{};
    ok(api.GetWindowState(&window));
    ok(scene.draw(api, 0, window.drawableWidth, window.drawableHeight));
    ok(api.Present(nullptr));
  };
  auto settled = remix::test::preparedFrame(frame);
  using Clock = std::chrono::steady_clock;
  if (std::getenv("REMIX_TEST_LAST_FRAME")) {
    require(argc > 1, "Missing capture directory");
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    auto capture = [&](const char *name) {
      const auto path = (directory / name).string();
      ok(api.RequestPresentedScreenshot(path.c_str()));
      frame();
      frame();
      int width = 0, height = 0, channels = 0;
      auto *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
      require(pixels && width > 0 && height > 0, "Missing captured image");
      std::vector<unsigned char> image(pixels, pixels + size_t(width) * height * 4);
      stbi_image_free(pixels);
      return std::tuple{width, height, image};
    };
    ok(api.SetConfigVariable("rtx.native.backend", "raster"));
    settled();
    const auto [width, height, reference] = capture("reference.png");
    // Capture the game behind an open F12 panel, then freeze preparation and
    // close the panel: no old controls may be baked into the retained image.
    ok(api.SetUIState(REMIXAPI_UI_STATE_ADVANCED));
    frame();
    holdCompile = true;
    entered = false;
    ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
    auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!entered) {
      frame();
      require(Clock::now() < deadline, "Compiler not entered for retained frame");
    }
    require(runtime.preparationStatus().loading, "Expected loading over retained frame");
    ok(api.SetUIState(REMIXAPI_UI_STATE_NONE));
    const auto before = runtime.presentationCount();
    for (unsigned i = 0; i < 20; ++i)
      frame();
    auto verify = [&](const char *name) {
      const auto [w, h, actual] = capture(name);
      unsigned checked = 0, changed = 0;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          // The preparation overlay is live in the middle of the image.
          if (std::abs(x - w / 2) < 160 && std::abs(y - h / 2) < 100)
            continue;
          const auto source = (size_t(y * height / h) * width + x * width / w) * 4;
          const auto destination = (size_t(y) * w + x) * 4;
          ++checked;
          for (int c = 0; c < 3; ++c)
            if (std::abs(int(actual[destination + c]) - int(reference[source + c])) > 2) {
              ++changed;
              break;
            }
        }
      require(changed < checked / 100, "Loading replaced or contaminated the last game frame");
    };
    verify("retained.png");
    ok(api.SetUIState(REMIXAPI_UI_STATE_ADVANCED));
    frame();
    require(api.GetUIState() != REMIXAPI_UI_STATE_NONE, "F12 unavailable over retained frame");
    capture("retained-f12.png");
    ok(api.SetUIState(REMIXAPI_UI_STATE_NONE));
    ok(api.SetConfigVariable("rtx.native.vsync", "true"));
    verify("retained-vsync.png");
    require(SDL_SetWindowSize(runtime.window().handle(), width * 2, height * 2), "Resize failed");
    for (int i = 0; i < 10; ++i)
      frame();
    verify("retained-resized.png");
    require(runtime.presentationCount() > before + 30, "Retained frame stopped presentation");
    holdCompile = false;
    gate.notify_all();
    settled();
    require(runtime.backendCapabilities().activeBackend == 1, "Replacement did not activate");
    capture("activated.png");
    ok(api.Shutdown());
    std::puts("Last game frame survives blocked compilation, live F12, VSync "
              "and resize; replacement activates");
    return 0;
  }
  auto measure = [&](const char *phase, auto change) {
    const bool startupPhase = std::string(phase) == "startup";
    const auto before = startupPhase ? remix::pipeline::Statistics{} : runtime.preparationStatus().pipelines;
    const auto start = startupPhase ? applicationStart : Clock::now();
    auto last = Clock::time_point{};
    const auto firstPresentation = runtime.presentationCount();
    auto previousPresentation = firstPresentation;
    double gap = 0;
    uint64_t frames = 0;
    change();
    do {
      frame();
      const auto now = Clock::now();
      if (runtime.presentationCount() != previousPresentation) {
        if (last != Clock::time_point{})
          gap = std::max(gap, std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
        previousPresentation = runtime.presentationCount();
      }
      frames = runtime.presentationCount() - firstPresentation;
      require(runtime.preparationStatus().failure.empty(), "Benchmark preparation failed");
      require(now - start < std::chrono::seconds(600), "Preparation timed out");
    } while (runtime.preparationStatus().pending);
    const auto after = runtime.preparationStatus().pipelines;
    std::printf("%s,%.3f,%llu,%llu,%llu,%llu,%.3f\n", phase,
                std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                (unsigned long long)(after.created - before.created),
                (unsigned long long)(after.reused - before.reused),
                (unsigned long long)(after.cacheHits - before.cacheHits), (unsigned long long)frames, gap);
    std::fflush(stdout);
    return after.created - before.created;
  };
  std::puts("phase,preparation_ms,created,reused,cache_hits,presentations,"
            "longest_gap_ms");
  if (!std::getenv("REMIX_TEST_PREPARATION_BENCHMARK")) {
    holdCompile = true;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!entered) {
      frame();
      require(Clock::now() < deadline, "Startup compiler not entered");
    }
    require(runtime.preparationStatus().loading, "Cold startup did not show loading screen");
    for (int i = 0; i < 10; ++i)
      frame();
    if (argc > 1) {
      std::filesystem::create_directories(argv[1]);
      const auto path = (std::filesystem::path(argv[1]) / "loading.png").string();
      ok(api.RequestPresentedScreenshot(path.c_str()));
      frame();
      frame();
    }
    holdCompile = false;
    gate.notify_all();
  }
  measure("startup", [] {});
  if (std::getenv("REMIX_TEST_PREPARATION_BENCHMARK")) {
    measure("first_nrd", [&] {
      config("denoiser", "reblur-sh");
      config("taa", "true");
    });
    measure("raw", [&] {
      config("denoiser", "off");
      config("taa", "false");
    });
    const auto repeated = measure("repeat_nrd", [&] {
      config("denoiser", "reblur-sh");
      config("taa", "true");
    });
    require(!repeated, "Repeated toggle recompiled pipelines");
    measure("first_fsr", [&] { config("upscaler", "fsr31"); });
    measure("fsr_off", [&] { config("upscaler", "off"); });
    require(!measure("repeat_fsr", [&] { config("upscaler", "fsr31"); }), "Repeated FSR recompiled");
    ok(api.Shutdown());
    return 0;
  }
  entered = false;
  holdCompile = true;
  config("denoiser", "reblur-sh");
  config("taa", "true");
  auto deadline = Clock::now() + std::chrono::seconds(30);
  while (!entered) {
    frame();
    require(Clock::now() < deadline, "Compiler not entered");
  }
  const auto before = runtime.presentationCount();
  const auto start = Clock::now();
  for (unsigned i = 0; i < 100; ++i) {
    if (i == 20) {
      SDL_Event event{};
      event.type = SDL_EVENT_KEY_DOWN;
      event.key.windowID = SDL_GetWindowID(runtime.window().handle());
      event.key.scancode = SDL_SCANCODE_F12;
      event.key.key = SDLK_F12;
      event.key.down = true;
      require(SDL_PushEvent(&event), "F12 push failed");
    }
    frame();
    require(!runtime.denoisingStatistics().activeDenoiser, "Premature activation");
    require(!runtime.preparationStatus().loading, "Working renderer replaced by loading screen");
  }
  require(runtime.presentationCount() >= before + 100, "Presentation stopped during compilation");
  require(api.GetUIState() != REMIXAPI_UI_STATE_NONE, "F12 blocked by compilation");
  require(Clock::now() - start < std::chrono::seconds(5), "Compiler stalled frame submission");
  if (argc > 1) {
    std::filesystem::create_directories(argv[1]);
    const auto path = (std::filesystem::path(argv[1]) / "preparing.png").string();
    ok(api.RequestPresentedScreenshot(path.c_str()));
    frame();
    frame();
  }
  // Supersede the executing NRD request, then return to the raw configuration
  // with a different sample count. The obsolete result must never activate.
  config("denoiser", "off");
  config("taa", "false");
  config("samples", "3");
  frame();
  holdCompile = false;
  gate.notify_all();
  settled();
  require(!runtime.denoisingStatistics().activeDenoiser && runtime.tracingStatistics().samplesPerPixel == 3,
          "Obsolete configuration activated");
  config("denoiser", "reblur-sh");
  config("taa", "true");
  settled();
  require(runtime.denoisingStatistics().activeDenoiser, "NRD did not activate");
  const auto compiled = runtime.preparationStatus().pipelines.created;
  config("denoiser", "off");
  config("taa", "false");
  settled();
  config("denoiser", "reblur-sh");
  config("taa", "true");
  settled();
  require(runtime.preparationStatus().pipelines.created == compiled, "Repeated settings recompiled pipelines");
  failCompile = true;
  config("sigma", "true");
  deadline = Clock::now() + std::chrono::seconds(30);
  while (runtime.preparationStatus().failure.empty()) {
    frame();
    require(Clock::now() < deadline, "Missing compilation failure");
  }
  require(runtime.denoisingStatistics().activeDenoiser && !runtime.shadowStatistics().active,
          "Failure replaced the working configuration");
  const auto failurePresentations = runtime.presentationCount();
  for (int i = 0; i < 10; ++i)
    frame();
  require(runtime.presentationCount() >= failurePresentations + 10, "Failure stopped gameplay");
  config("sigma", "false");
  frame();
  failImage = true;
  config("upscaler", "fsr31");
  deadline = Clock::now() + std::chrono::seconds(30);
  // Starting a different request clears the previous compilation failure.
  frame();
  while (runtime.preparationStatus().failure.empty()) {
    frame();
    require(Clock::now() < deadline, "Missing resource allocation failure");
  }
  require(runtime.denoisingStatistics().activeDenoiser &&
              runtime.reconstructionStatistics().backend == remix::upscale::Backend::Native,
          "Allocation failure replaced the working configuration");
  config("upscaler", "off");
  settled();
  require(runtime.preparationStatus().failure.empty(), "Returning to the active configuration retained a failure");
  config("upscaler", "fsr31");
  settled();
  require(runtime.reconstructionStatistics().backend == remix::upscale::Backend::Fsr31,
          "Reselecting a failed allocation did not retry preparation");
  ok(api.Shutdown());
  std::puts("Blocked driver compilation preserved presentation/F12; latest "
            "request, reuse and failure retention passed");
}
