#include <remix/remix_native_ext.h>
#include <remix/remix_reconstruction_ext.h>
#include <remix/remix_particles_ext.h>
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void ok(remixapi_ErrorCode result) { require(result == REMIXAPI_ERROR_CODE_SUCCESS, "API failed"); }
} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "Supply screenshot output directory");
  static_assert(sizeof(remixapi_MeshInfoUpdateEXT) == 24);
  static_assert(sizeof(remixapi_MeshInfoMotionEXT) == 24);
  static_assert(offsetof(remixapi_MeshInfoMotionEXT, enabled) == 16);
  static_assert(offsetof(remixapi_MeshInfoUpdateEXT, enabled) == 16);
  static_assert(sizeof(remixapi_InputEvent) == 32);
  static_assert(sizeof(remixapi_WindowMetrics) == 16);
  static_assert(sizeof(remixapi_CameraTemporalEXT) == 32);
  static_assert(sizeof(remixapi_CameraEnvironmentEXT) == 32);
  static_assert(offsetof(remixapi_CameraEnvironmentEXT, enabled) == 16);
  static_assert(offsetof(remixapi_CameraEnvironmentEXT, radiance) == 20);
  static_assert(sizeof(remixapi_DenoisingCapabilitiesEXT) == 272);
  static_assert(sizeof(remixapi_DenoisingStatisticsEXT) == 344);
  static_assert(sizeof(remixapi_FramePerformanceEXT) == 176);
  remixapi_FramePerformanceEXT framePerformance{};
  require(remixapi_GetFramePerformanceEXT(nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Null frame performance accepted");
  require(remixapi_GetFramePerformanceEXT(&framePerformance) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid frame performance size accepted");
  framePerformance.structSize = sizeof(framePerformance);
  require(remixapi_GetFramePerformanceEXT(&framePerformance) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "Frame performance before startup");
  static_assert(sizeof(remixapi_FrameBreakdownEXT) == 256);
  remixapi_FrameBreakdownEXT frameBreakdown{};
  require(remixapi_GetFrameBreakdownEXT(&frameBreakdown) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid frame breakdown size accepted");
  frameBreakdown.structSize = sizeof(frameBreakdown);
  require(remixapi_GetFrameBreakdownEXT(&frameBreakdown) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "Frame breakdown before startup");
  static_assert(sizeof(remixapi_GpuPassTimingEXT) == 88);
  static_assert(sizeof(remixapi_GpuPassTimingsEXT) == 80 + 88 * REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT);
  static remixapi_GpuPassTimingsEXT passTimings{};
  require(remixapi_GetGpuPassTimingsEXT(&passTimings) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid GPU pass timings size accepted");
  passTimings.structSize = sizeof(passTimings);
  require(remixapi_GetGpuPassTimingsEXT(&passTimings) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "GPU pass timings before startup");
  remixapi_DenoisingCapabilitiesEXT denoiseCaps{};
  remixapi_DenoisingStatisticsEXT denoiseStats{};
  require(remixapi_GetDenoisingCapabilitiesEXT(nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Null denoising capabilities accepted");
  require(remixapi_GetDenoisingStatisticsEXT(&denoiseStats) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid denoising struct size accepted");
  denoiseCaps.structSize = sizeof(denoiseCaps);
  denoiseStats.structSize = sizeof(denoiseStats);
  require(remixapi_GetDenoisingCapabilitiesEXT(&denoiseCaps) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "Denoising capability before startup");
  require(remixapi_GetDenoisingStatisticsEXT(&denoiseStats) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "Denoising statistics before startup");
  uint32_t count = 99;
  require(remixapi_PollInputEvents(nullptr, 0, &count) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED && !count,
          "Input before startup");
  require(remixapi_PollInputEvents(nullptr, 1, &count) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Missing event array accepted");
  remixapi_InitializeLibraryInfo initialize = {};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  remixapi_Interface api = {};
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = 320;
  startup.windowHeight = 240;
#endif
  ok(api.Startup(&startup));
  static_assert(sizeof(remixapi_ReconstructionDiagnosticsEXT) == 336);
  static_assert(offsetof(remixapi_ReconstructionDiagnosticsEXT, memoryBytes) == 64);
  remixapi_ReconstructionDiagnosticsEXT reconstruction{};
  require(remixapi_GetReconstructionDiagnosticsEXT(nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "Null reconstruction accepted");
  require(remixapi_GetReconstructionDiagnosticsEXT(&reconstruction) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "Zero diagnostics capacity accepted");
  // Prefix callers must not have their adjacent data overwritten.
  struct { uint32_t size, version; uint64_t sentinel; } prefix{8,0,0x12345678abcdefull};
  ok(remixapi_GetReconstructionDiagnosticsEXT(reinterpret_cast<remixapi_ReconstructionDiagnosticsEXT *>(&prefix)));
  require(prefix.version == 1 && prefix.size == sizeof(reconstruction) && prefix.sentinel == 0x12345678abcdefull, "Diagnostics prefix write exceeded capacity");
  reconstruction.structSize = sizeof(reconstruction);
  ok(remixapi_GetReconstructionDiagnosticsEXT(&reconstruction));
  require(reconstruction.requestedMode == 1 && !reconstruction.activeBackend, "Default reconstruction request changed");
  ok(remixapi_GetFramePerformanceEXT(&framePerformance));
  require(!framePerformance.attempts && !framePerformance.presentations,
          "Frame performance not reset on startup");
  ok(remixapi_GetDenoisingCapabilitiesEXT(&denoiseCaps));
  ok(remixapi_GetDenoisingStatisticsEXT(&denoiseStats));
  require(denoiseStats.requestedDenoiser && denoiseStats.requestedTaa && !denoiseStats.activeDenoiser &&
              !denoiseStats.activeTaa && !denoiseStats.memoryBytes,
          "Raster allocated or activated temporal resources");
  remixapi_FeatureAvailability features{};
  ok(api.GetFeatureAvailability(&features));
  require(bool(features.availableFeatures & REMIXAPI_FEATURE_NRD_BIT) == bool(denoiseCaps.reblurShSupported),
          "NRD availability depends on selected renderer");
  std::printf("Independent temporal capability: REBLUR=%u TAA=%u reason=%s\n", denoiseCaps.reblurShSupported,
              denoiseCaps.taaSupported, denoiseCaps.unavailableReason);
  int number = 0;
  SDL_Window **windows = SDL_GetWindows(&number);
  require(number == 1, "Expected one runtime window");
  SDL_Window *window = windows[0];
  SDL_free(windows);
  ok(api.PumpEvents());
  std::array<remixapi_InputEvent, 4096> events;
  ok(remixapi_PollInputEvents(events.data(), events.size(), &count));
  ok(remixapi_SetTextInputEnabled(1));
  SDL_Event event = {};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.windowID = SDL_GetWindowID(window);
  event.key.scancode = SDL_SCANCODE_A;
  event.key.down = true;
  require(SDL_PushEvent(&event), "Push key down");
  event.key.repeat = true;
  require(SDL_PushEvent(&event), "Push repeat");
  event.type = SDL_EVENT_KEY_UP;
  event.key.down = false;
  event.key.repeat = false;
  require(SDL_PushEvent(&event), "Push key up");
  event = {};
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.windowID = SDL_GetWindowID(window);
  event.text.text = "\xc3\xa9\xf0\x9f\x98\x80";
  require(SDL_PushEvent(&event), "Push Unicode");
  ok(api.PumpEvents());
  ok(remixapi_PollInputEvents(nullptr, 0, &count));
  require(count == 5, "Input query consumed or lost events");
  ok(remixapi_PollInputEvents(events.data(), 2, &count));
  require(count == 2 && events[0].code == REMIXAPI_KEY_A && events[0].down && !events[0].repeat &&
              events[1].repeat,
          "Key/repeat ordering");
  ok(remixapi_PollInputEvents(events.data(), events.size(), &count));
  require(count == 3 && !events[0].down && events[1].type == REMIXAPI_INPUT_TEXT && events[1].code == 0xe9 &&
              events[2].code == 0x1f600,
          "Release or Unicode lost");
  std::thread wrongThread([] {
    uint32_t count = 1;
    require(remixapi_PollInputEvents(nullptr, 0, &count) == REMIXAPI_ERROR_CODE_WRONG_THREAD && !count,
            "Input thread ownership");
    require(remixapi_SetTextInputEnabled(0) == REMIXAPI_ERROR_CODE_WRONG_THREAD,
            "Text input thread ownership");
  });
  wrongThread.join();
  event = {};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.windowID = SDL_GetWindowID(window);
  for (int i = 0; i < 4100; ++i)
    require(SDL_PushEvent(&event), "Push overflow events");
  ok(api.PumpEvents());
  ok(remixapi_PollInputEvents(events.data(), events.size(), &count));
  require(count && count <= 4096 && events[0].type == REMIXAPI_INPUT_RESET, "Overflow reset missing");
  remixapi_WindowMetrics metrics = {};
  ok(remixapi_GetWindowMetrics(&metrics));
  require(metrics.logicalWidth && metrics.logicalHeight && metrics.drawableWidth && metrics.drawableHeight,
          "Window metrics empty");
  std::filesystem::create_directories(argv[1]);
  const auto path = std::filesystem::absolute(std::filesystem::path(argv[1]) / "presented.png");
  std::filesystem::remove(path);
  const std::string filename = path.string();
  require(api.RequestPresentedScreenshot("relative.png") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Relative screenshot path accepted");
  ok(api.RequestPresentedScreenshot(filename.c_str()));
  require(api.RequestPresentedScreenshot(filename.c_str()) == REMIXAPI_ERROR_CODE_ALREADY_EXISTS,
          "Pending screenshot overwritten");
  // The request must survive swapchain recreation before the capture.
  require(SDL_SetWindowSize(window, 400, 300), "Resize before screenshot");
  // X11/window managers apply resize asynchronously; capture the completed resize.
  require(SDL_SyncWindow(window), "Wait for resize before screenshot");
  ok(api.PumpEvents());
  ok(api.Present(nullptr));
  ok(remixapi_GetFramePerformanceEXT(&framePerformance));
  require(framePerformance.attempts == 1 && framePerformance.presentMilliseconds >= 0 &&
              framePerformance.presentMilliseconds >= framePerformance.fenceMilliseconds &&
              framePerformance.presentations == framePerformance.presented,
          "Invalid frame performance accounting");
  for (unsigned i = 0; i < 250 && !std::filesystem::exists(path); ++i) {
    SDL_Delay(8);
    ok(api.PumpEvents());
  }
  int width, height, channels;
  unsigned char *pixels = stbi_load(filename.c_str(), &width, &height, &channels, 4);
  ok(remixapi_GetWindowMetrics(&metrics));
  require(pixels && width == int(metrics.drawableWidth) && height == int(metrics.drawableHeight),
          "Screenshot missing or wrong dimensions");
  require(pixels[0] > 30 && pixels[0] < 60 && pixels[1] > 40 && pixels[1] < 65 && pixels[2] > 55 &&
              pixels[2] < 80 && pixels[3] == 255,
          "Screenshot color/BGRA conversion");
  stbi_image_free(pixels);
  {
    // Particle emitters: a burst in front of the camera is simulated and
    // counted once its frames complete.
    remixapi_CameraInfoParameterizedEXT parameters{};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.right = {1, 0, 0};
    parameters.up = {0, 1, 0};
    parameters.forward = {0, 0, 1};
    parameters.aspect = 4.f / 3;
    parameters.fovYInDegrees = 90;
    parameters.nearPlane = .1f;
    parameters.farPlane = 100;
    remixapi_CameraInfo camera{};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    remixapi_ParticleEmitterInfoEXT emitter{};
    emitter.sType = REMIXAPI_STRUCT_TYPE_PARTICLE_EMITTER_INFO_EXT;
    emitter.shape = REMIXAPI_PARTICLE_SHAPE_SPHERE_EXT;
    emitter.flags = REMIXAPI_PARTICLE_UNLIT_EXT;
    emitter.maxParticles = 256;
    emitter.transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 5}}};
    emitter.extent = {1, 1, 1};
    emitter.lifetimeMin = emitter.lifetimeMax = 60;
    emitter.sizeStart = emitter.sizeEnd = .2f;
    emitter.colorStart = emitter.colorEnd = {1, 1, 1, 1};
    remixapi_ParticleEmitterHandleEXT handle = 0;
    ok(remixapi_CreateParticleEmitterEXT(&emitter, &handle));
    emitter.maxParticles = 70000;
    require(remixapi_UpdateParticleEmitterEXT(handle, &emitter) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "Too many particles accepted");
    ok(remixapi_BurstParticleEmitterEXT(handle, 100));
    remixapi_ParticleStatisticsEXT particles{};
    particles.structSize = sizeof(particles);
    for (int frame = 0; frame < 30 && !particles.liveParticles; ++frame) {
      ok(api.SetupCamera(&camera));
      ok(api.Present(nullptr));
      ok(remixapi_GetParticleStatisticsEXT(&particles));
    }
    require(particles.emitters == 1 && particles.liveParticles == 100 && particles.allocatedParticles == 256,
            "Burst particles were not simulated");
    ok(remixapi_DestroyParticleEmitterEXT(handle));
    require(remixapi_DestroyParticleEmitterEXT(handle) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "A destroyed emitter was destroyed again");
  }
  ok(api.Shutdown());
  count = 99;
  require(remixapi_PollInputEvents(nullptr, 0, &count) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED && !count,
          "Shutdown retained input");
  return 0;
}
