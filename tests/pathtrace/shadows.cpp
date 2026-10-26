#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void ok(remixapi_ErrorCode result) { require(!result, "Shadow test API failure"); }
}
int main(int argc, char **argv) {
  require(argc == 2, "Supply screenshot directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = 256; startup.windowHeight = 128;
  if (std::getenv("REMIX_TEST_SHADOW_BENCHMARK")) {
    startup.windowWidth = 1920; startup.windowHeight = 1080;
  }
#endif
  ok(api.Startup(&startup));
  // These fixtures measure the unexposed lighting/display contract.
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "off"));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2)) {
    std::printf("Sky scenes unavailable: %s\n", caps.unavailableReason);
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  // These radiometric checks decode the gamma 2.2 output.
  ok(api.SetConfigVariable("rtx.native.pathtrace.samples", "16"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "1"));
  require(api.SetConfigVariable("rtx.native.pathtrace.sky", "2") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid sky toggle accepted");
  uint64_t next = 100;
  auto material = [&](remixapi_Float3D color, float alpha, bool cutout = false) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = cutout ? 4 : 7; // GREATER .5
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = next++;
    remixapi_MaterialHandle handle{};
    ok(api.CreateMaterial(&info, &handle));
    return handle;
  };
  const auto white = material({1, 1, 1}, 1), black = material({0, 0, 0}, 1);
  const auto cutout = material({1, 1, 1}, 0, true), tint = material({.1f, 1, .2f}, .5f);
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1, remixapi_MaterialHandle mat) {
    remixapi_HardcodedVertex vertices[4]{};
    for (int i = 0; i < 4; ++i) {
      vertices[i].position[axis] = plane;
      vertices[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      vertices[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      vertices[i].color = 0xffffffff;
    }
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices; surface.vertices_count = 4;
    surface.indices_values = indices; surface.indices_count = 6; surface.material = mat;
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = next++; mesh.surfaces_values = &surface; mesh.surfaces_count = 1;
    remixapi_MeshHandle handle{};
    ok(api.CreateMesh(&mesh, &handle));
    return handle;
  };

  const auto receiver = quad(2, .5f, -2, 2, -2, 2, white);
  const auto blocker = quad(2, -.5f, -20, 0, -20, 20, black);
  const auto holes = quad(2, -.5f, -20, 0, -20, 20, cutout);
  const auto glass = quad(2, -.5f, -20, 0, -20, 20, tint);
  remixapi_MeshHandle occluder = blocker;
  float separation = 8, shift = 0;
  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1;
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1; epoch.pNext = &environment;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO; camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i) camera.view[i][i] = camera.projection[i][i] = 1;
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, 0, 1};
  distant.angularDiameterDegrees = 3;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; light.pNext = &distant; light.hash = next++;
  light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&light, &sun));
  auto configure = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  const bool blueNoise = !std::getenv("REMIX_TEST_WHITE_SHADOWS");
  configure("blueNoiseShadows", blueNoise ? "1" : "0");
  require(api.SetConfigVariable("rtx.native.pathtrace.blueNoiseShadows", "2") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid blue-noise shadow toggle accepted");
  auto draw = [&](remixapi_MeshHandle mesh) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = reinterpret_cast<uintptr_t>(mesh); identity.rasterVisible = 1;
    identity.classification = mesh == receiver ? 0 : 1;
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT; blend.pNext = &identity;
    blend.alphaBlendEnabled = mesh == glass;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO; instance.pNext = &blend;
    instance.mesh = mesh; instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i) instance.transform.matrix[i][i] = 1;
    if (mesh != receiver) {
      instance.transform.matrix[2][3] = 1 - separation;
      instance.transform.matrix[0][3] = shift;
    }
    ok(api.DrawInstance(&instance));
  };
  bool continuous = false;
  unsigned refresh = 0;
  auto frame = remix::test::preparedFrame([&] {
    // This scene has no local emitters. Toggling their unused proposal setting
    // keeps scene submissions fresh without changing sun samples or history.
    // Otherwise the renderer legitimately reuses its converged static image.
    if (continuous)
      configure("emissiveSolidAngleSampling", (++refresh & 1) ? "1" : "0");
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    draw(receiver);
    if (occluder)
      draw(occluder);
    ok(api.Present(nullptr));
  });
  auto shadows = [&] {
    remixapi_ShadowStatisticsEXT stats{}; stats.structSize = sizeof(stats);
    ok(remixapi_GetShadowStatisticsEXT(&stats)); return stats;
  };
  auto temporal = [&] {
    remixapi_DenoisingStatisticsEXT stats{}; stats.structSize = sizeof(stats);
    ok(remixapi_GetDenoisingStatisticsEXT(&stats)); return stats;
  };
  auto capture = [&](const std::string &name, unsigned count = 40) {
    for (unsigned i = 0; i < count; ++i) frame();
    const auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int width, height, channels;
    auto pixels = stbi_load(file.c_str(), &width, &height, &channels, 4);
    require(pixels && width == 256 && height == 128, "Missing shadow screenshot");
    std::vector<std::array<float, 3>> result(width * height);
    for (int i = 0; i < width * height; ++i)
      for (int c = 0; c < 3; ++c) {
        float v = pixels[i * 4 + c] / 255.f;
        result[i][c] = std::pow(v, 2.2f);
      }
    stbi_image_free(pixels);
    return result;
  };
  auto average = [](const auto &pixels, int x0, int x1, int channel = 0) {
    double result = 0;
    for (int y = 32; y < 96; ++y)
      for (int x = x0; x < x1; ++x) result += pixels[y * 256 + x][channel];
    return result / (64 * (x1 - x0));
  };
  auto width = [&](const auto &pixels) {
    unsigned result = 0;
    for (int x = 64; x < 192; ++x) {
      double value = average(pixels, x, x + 1);
      result += value > .1 && value < .9;
    }
    return result;
  };
  auto mse = [](const auto &pixels) {
    double result = 0;
    const double radius = 8 * std::tan(3 * 3.14159265359 / 360);
    for (int y = 32; y < 96; ++y) for (int x = 96; x < 160; ++x) {
      double position = ((x + .5) / 128 - 1) / radius;
      position = std::clamp(position, -1., 1.);
      double analytic = .5 + (std::asin(position) + position * std::sqrt(1 - position * position)) / 3.14159265359;
      double error = pixels[y * 256 + x][0] - analytic;
      result += error * error;
    }
    return result / 4096;
  };
  configure("samples", "1"); configure("bounces", "0");
  configure("sharc", "0"); configure("denoiser", "reblur-sh"); configure("taa", "1");
  ok(api.SetConfigVariable("rtx.native.vsync", "False"));
  remixapi_ShadowStatisticsEXT invalid{};
  require(remixapi_GetShadowStatisticsEXT(nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS &&
          remixapi_GetShadowStatisticsEXT(&invalid) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Malformed shadow diagnostics accepted");
  for (auto value : {"nan", "inf", "-2", "11", "0x1", "1x"})
    require(api.SetConfigVariable("rtx.native.pathtrace.sunAngularDiameter", value) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "Malformed sun diameter override accepted");
  frame(); frame();
  if (const char *fault = std::getenv("REMIX_TEST_FAULT")) {
    if (std::strcmp(fault, "blue-noise-allocation") == 0) {
      require(shadows().active && temporal().activeDenoiser && temporal().activeTaa,
              "Blue-noise allocation failure disabled denoising");
      configure("shadowDebug", "2");
      const auto fallback = capture("blue-noise-fallback");
      require(average(fallback, 32, 64) < .01 && average(fallback, 192, 224) > .98 && mse(fallback) < .02,
              "Blue-noise allocation fallback changed sun visibility");
      std::puts("Blue-noise allocation fallback retained SIGMA and shadow energy");
      ok(api.Shutdown()); return 0;
    }
    require(!shadows().active && shadows().unavailableReason[0] && temporal().activeDenoiser && temporal().activeTaa,
            "SIGMA failure disabled the existing indirect/TAA path");
    auto image = capture("fallback");
    require(average(image, 192, 224) > .9, "SIGMA fallback lost direct lighting");
    std::printf("%s retained REBLUR SH and TAA\n", fault);
    ok(api.Shutdown()); return 0;
  }
  if (!shadows().supported) {
    std::printf("SIGMA unavailable: %s\n", shadows().unavailableReason);
    ok(api.Shutdown()); return 77;
  }
  require(shadows().active, "Supported SIGMA did not activate");
  if (std::getenv("REMIX_TEST_SHADOW_BENCHMARK")) {
    continuous = true;
    auto output = std::fopen((directory / "timings.csv").string().c_str(), "w");
    require(output, "Cannot write shadow benchmark");
    std::fprintf(output, "phase,blue_noise,frame,gpu_trace_ms,gpu_sigma_ms,present_ms\n");
    unsigned phase = 0;
    for (bool enabled : {false, true, true, false}) {
      configure("blueNoiseShadows", enabled ? "1" : "0");
      for (unsigned i = 0; i < 64; ++i) frame();
      uint64_t previousSequence = 0;
      for (unsigned i = 0; i < 256; ++i) {
        auto begin = std::chrono::steady_clock::now();
        frame();
        double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        remixapi_TracingStatisticsEXT tracing{}; tracing.structSize = sizeof(tracing);
        ok(remixapi_GetTracingStatisticsEXT(&tracing));
        require(tracing.gpuTimingsAvailable && shadows().gpuTimingsAvailable, "GPU benchmark timestamps unavailable");
        require(!previousSequence || tracing.sampleSequence > previousSequence, "Benchmark reused a converged frame");
        previousSequence = tracing.sampleSequence;
        std::fprintf(output, "%u,%u,%u,%.6f,%.6f,%.6f\n", phase, unsigned(enabled), i,
                     tracing.gpuTraceMilliseconds, shadows().gpuSigmaMilliseconds, milliseconds);
      }
      ++phase;
    }
    std::fclose(output);
    ok(api.Shutdown());
    std::puts("Shadow sampler 1080p comparison captured");
    return 0;
  }
  auto metrics = std::fopen((directory / "blue-noise.csv").string().c_str(), "w");
  require(metrics, "Cannot write shadow comparison metrics");
  std::fprintf(metrics, "primary,blue_noise,frame,sequence,mse,mean,temporal_mse,gpu_trace_ms,gpu_sigma_ms\n");
  for (const char *primary : {"raster", "ray"}) {
    configure("primaryVisibility", primary);
    configure("denoiser", "off"); configure("taa", "0");
    configure("shadowDebug", "2");
    distant.direction = {0, 0, 1}; distant.angularDiameterDegrees = 3;
    ok(api.UpdateLightDefinition(sun, &light));
    separation = 1; occluder = blocker; ++epoch.historyResetEpoch;
    auto near = capture(std::string(primary) + "-near");
    separation = 8; ++epoch.historyResetEpoch;
    auto far = capture(std::string(primary) + "-far");
    unsigned nearWidth = width(near), farWidth = width(far);
    std::printf("%s penumbra 10-90%%: near=%u far=%u pixels\n", primary, nearWidth, farWidth);
    require(farWidth > nearWidth * 2 && farWidth > 10, "Penumbra did not grow with blocker distance");
    require(average(far, 32, 64) < .01 && average(far, 192, 224) > .98,
            "SIGMA changed fully shadowed/lit regions");
    // Complete eight-sample cycles after warmup, including every temporal phase.
    continuous = true;
    auto previous = capture(std::string(primary) + "-continuous-warmup", 40);
    uint64_t previousSequence = 0;
    for (unsigned i = 0; i < 16; ++i) {
      auto image = capture(std::string(primary) + "-sequence-" + std::to_string(i), 1);
      double difference = 0;
      for (int y = 32; y < 96; ++y) for (int x = 96; x < 160; ++x) {
        const double delta = image[y * 256 + x][0] - previous[y * 256 + x][0];
        difference += delta * delta;
      }
      remixapi_TracingStatisticsEXT tracing{}; tracing.structSize = sizeof(tracing);
      ok(remixapi_GetTracingStatisticsEXT(&tracing));
      require(!previousSequence || tracing.sampleSequence > previousSequence, "Shadow comparison reused a converged frame");
      previousSequence = tracing.sampleSequence;
      std::fprintf(metrics, "%s,%u,%u,%llu,%.9f,%.9f,%.9f,%.6f,%.6f\n", primary, unsigned(blueNoise), i,
                   static_cast<unsigned long long>(tracing.sampleSequence),
                   mse(image), average(image, 96, 160), difference / 4096,
                   tracing.gpuTraceMilliseconds, shadows().gpuSigmaMilliseconds);
      previous = std::move(image);
    }
    continuous = false;
    configure("shadowDebug", "1");
    auto raw = capture(std::string(primary) + "-raw-visibility");
    std::printf("%s analytic disk MSE: raw=%.6f SIGMA=%.6f\n", primary, mse(raw), mse(far));
    require(mse(far) < mse(raw) * .5, "SIGMA did not reduce sampled penumbra noise");
    configure("shadowDebug", "0"); configure("accumulate", "1");
    auto progressive = capture(std::string(primary) + "-progressive", 128);
    require(!shadows().active && mse(progressive) < .01, "Raw progressive sun sampling differs from disk reference");
    configure("accumulate", "0");
    configure("shadowDebug", "2"); configure("sunAngularDiameter", "0");
    auto hard = capture(std::string(primary) + "-hard");
    require(width(hard) <= 2, "Zero diameter did not retain hard shadows");
    configure("sunAngularDiameter", "-1");
    occluder = holes; ++epoch.historyResetEpoch;
    auto cut = capture(std::string(primary) + "-cutout");
    require(average(cut, 32, 224) > .98, "Rejected alpha cutout blocked sunlight");
    occluder = glass; ++epoch.historyResetEpoch;
    auto tinted = capture(std::string(primary) + "-tinted");
    const double red = average(tinted, 32, 64, 0), green = average(tinted, 32, 64, 1), blue = average(tinted, 32, 64, 2);
    std::printf("%s tinted umbra %.4f %.4f %.4f\n", primary, red, green, blue);
    require(green > .4 && green < .6 && red < green * .2 && blue < green * .4,
            "SIGMA lost tinted transparent occlusion");
    configure("shadowDebug", "0"); configure("denoiser", "reblur-sh"); configure("taa", "1");
    configure("bounces", "2"); configure("sharc", "1"); configure("sharcCapacityLog2", "18");
    environment.radiance = {.15f, .15f, .15f}; occluder = blocker;
    capture(std::string(primary) + "-combined", 96);
    require(shadows().active && temporal().activeDenoiser && temporal().activeTaa,
            "SIGMA and diffuse SH/TAA did not run together");
    auto beforeToggle = temporal().historyResets, shadowBeforeToggle = shadows().historyResets;
    configure("blueNoiseShadows", blueNoise ? "0" : "1"); frame(); frame();
    require(temporal().historyResets == beforeToggle && shadows().historyResets > shadowBeforeToggle,
            "Shadow sampler toggle did not reset only SIGMA");
    configure("blueNoiseShadows", blueNoise ? "1" : "0"); frame(); frame();
    auto resets = temporal().historyResets, shadowResets = shadows().historyResets;
    for (int i = 0; i < 16; ++i) {
      distant.direction.x = i * .0001f;
      light.radiance.x = 3.14159265f - .001f * i;
      ok(api.UpdateLightDefinition(sun, &light)); frame();
    }
    require(temporal().historyResets == resets && shadows().historyResets == shadowResets,
            "Ordinary sun movement reset temporal history");
    distant.direction.x = .3f; ok(api.UpdateLightDefinition(sun, &light)); frame();
    require(temporal().historyResets == resets && shadows().historyResets > shadowResets,
            "Sun cut did not reset only SIGMA");
    shift = -3;
    auto moved = capture(std::string(primary) + "-moving-blocker", 96);
    require(average(moved, 64, 96) > .9, "Moving blocker left stale shadows");
    shift = 0;
    light.radiance = {}; ok(api.UpdateLightDefinition(sun, &light));
    configure("sharc", "0"); configure("bounces", "1");
    auto sigmaIndirect = capture(std::string(primary) + "-indirect-only", 96);
    require(average(sigmaIndirect, 192, 224) > .05, "SIGMA removed indirect light when the sun is disabled");
    configure("sigma", "0");
    auto indirectOnly = capture(std::string(primary) + "-indirect-no-sigma", 96);
    require(std::abs(average(sigmaIndirect, 192, 224) - average(indirectOnly, 192, 224)) < .025,
            "SIGMA changed the indirect signal");
    configure("sigma", "1"); configure("bounces", "0");
    auto zero = capture(std::string(primary) + "-no-sun-zero-bounce", 64);
    require(average(zero, 32, 224) < .005, "Disabled sunlight/zero bounces leaked lighting");
    light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
    ok(api.UpdateLightDefinition(sun, &light));
    environment.radiance = {};
  }
  std::printf("SIGMA last dispatch %.3f ms; shadow-only history resets %llu\n", shadows().gpuSigmaMilliseconds,
              static_cast<unsigned long long>(shadows().historyResets));
  std::fclose(metrics);
  ok(api.Shutdown());
  std::puts("SIGMA penumbra, occlusion, transmission, signal separation and temporal checks passed");
}
