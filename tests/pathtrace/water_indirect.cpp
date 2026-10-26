#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <remix/remix_reconstruction_ext.h>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void ok(remixapi_ErrorCode result) { require(!result, "Water test API failure"); }
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
  startup.windowWidth = std::getenv("REMIX_TEST_RR_REUSE") ? 256 : 79;
  startup.windowHeight = std::getenv("REMIX_TEST_RR_REUSE") ? 256 : 67; // Partial compute workgroups.
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
    std::printf("Water scenes unavailable: %s\n", caps.unavailableReason);
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
  const auto black = material({0, 0, 0}, 1);
  const auto tint = material({.1f, 1, .2f}, .5f);
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
  const auto receiver = quad(2, .8f, -2, 2, -2, 2, black);
  const auto glass = quad(2, -1, -1, 1, -1, 1, tint);
  remixapi_MaterialInfoTranslucentEXT waterInfo{};
  waterInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
  waterInfo.transmittanceColor = {.2f, .7f, .9f};
  remixapi_MaterialInfo waterMaterial{};
  waterMaterial.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  waterMaterial.pNext = &waterInfo; waterMaterial.hash = next++;
  remixapi_MaterialHandle waterHandle{}; ok(api.CreateMaterial(&waterMaterial, &waterHandle));
  const auto water = quad(2, .3f, -.85f, .85f, -.85f, .85f, waterHandle);
  const auto overlap = quad(2, .5f, .2f, .85f, -.85f, .85f, waterHandle);
  const auto ordinaryWater = quad(2, .3f, -.85f, .85f, -.85f, .85f, material({.2f, .7f, .9f}, .6f));
  const auto indirectBlocker = quad(2, -.25f, -2, -.1f, -2, 2, black);
  const auto shore = quad(2, .2f, -.85f, -.65f, -.85f, .85f, black);
  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1;
  environment.radiance = {.25f, .5f, .75f};
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1; epoch.pNext = &environment;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO; camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i) camera.view[i][i] = camera.projection[i][i] = 1;
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, 0, 1};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; light.pNext = &distant; light.hash = next++;
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&light, &sun));
  std::vector<remixapi_MeshHandle> enclosure;
  auto draw = [&](remixapi_MeshHandle mesh) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = reinterpret_cast<uintptr_t>(mesh); identity.rasterVisible = 1;
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT; blend.pNext = &identity;
    blend.alphaBlendEnabled = mesh == glass || mesh == water || mesh == overlap || mesh == ordinaryWater;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO; instance.pNext = &blend;
    instance.mesh = mesh; instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i) instance.transform.matrix[i][i] = 1;
    ok(api.DrawInstance(&instance));
  };
  bool reverse = false, forceFresh = true;
  unsigned tick = 0;
  auto frame = remix::test::preparedFrame([&] {
    camera.view[3][0] = forceFresh && (++tick & 1) ? .000001f : 0;
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    if (!reverse)
      draw(receiver);
    for (auto mesh : enclosure)
      draw(mesh);
    ok(api.Present(nullptr));
  });
  auto temporal = [&] {
    remixapi_DenoisingStatisticsEXT stats{}; stats.structSize = sizeof(stats);
    ok(remixapi_GetDenoisingStatisticsEXT(&stats)); return stats;
  };
  std::vector<std::array<float, 3>> region;
  auto readback = [&](const std::string &name) {
    const auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int width, height, channels;
    auto pixels = stbi_load(file.c_str(), &width, &height, &channels, 4);
    require(pixels, "Missing water screenshot");
    std::array<float, 3> average{};
    for (int y = height / 2 - 4; y < height / 2 + 4; ++y)
      for (int x = width / 2 - 4; x < width / 2 + 4; ++x)
        for (int c = 0; c < 3; ++c) {
          float v = pixels[(y * width + x) * 4 + c] / 255.f;
          average[c] += (std::pow(v, 2.2f)) / 64;
        }
    region.clear();
    for (int y = height / 4; y < height * 3 / 4; ++y)
      for (int x = width / 4; x < width * 3 / 4; ++x) {
        std::array<float, 3> value{};
        for (int c = 0; c < 3; ++c) {
          float v = pixels[(y * width + x) * 4 + c] / 255.f;
          value[c] = std::pow(v, 2.2f);
        }
        region.push_back(value);
      }
    stbi_image_free(pixels);
    std::printf("%s linear RGB: %.4f %.4f %.4f\n", name.c_str(), average[0], average[1], average[2]);
    return average;
  };
  auto capture = [&](const std::string &name, int frames) {
    // Independent scenes use explicit cuts; live-change checks below do not.
    ++epoch.historyResetEpoch;
    for (int i = 0; i < frames; ++i) frame();
    return readback(name);
  };
  auto config = [&](const char *name, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + name).c_str(), value));
  };
  config("sharc", "0"); config("localLights", "0"); config("sigma", "0");
  require(api.SetConfigVariable("rtx.native.pathtrace.waterIndirectReuse", "2") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid water reuse setting accepted");
  if (std::getenv("REMIX_TEST_RR_REUSE")) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) camera.projection[i][j] = 0;
    camera.projection[0][0] = camera.projection[1][1] = 1;
    camera.projection[2][2] = 100.f / 99.95f; camera.projection[2][3] = 1;
    camera.projection[3][2] = -5.f / 99.95f;
    config("samples", "1"); config("bounces", "1"); config("accumulate", "0");
    config("denoiser", "reblur-sh"); config("taa", "1"); config("dlssQuality", "native-aa");
    config("dlssRayReconstruction", "1"); config("upscaler", "dlss");
    enclosure = {water, shore, indirectBlocker};
    // Discovery is asynchronous and starts only when an NGX mode is requested.
    frame();
    remixapi_ReconstructionDiagnosticsEXT available{}; available.structSize = sizeof(available);
    ok(remixapi_GetReconstructionDiagnosticsEXT(&available));
    if (!(available.capabilities & 4)) { ok(api.Shutdown()); return 77; }
    for (const char *primary : {"raster", "ray"}) {
      config("primaryVisibility", primary);
      config("upscaler", "off"); config("accumulate", "1"); config("samples", "16");
      forceFresh = false;
      const auto reference = capture(std::string(primary) + "-accumulated-reference", 128);
      config("accumulate", "0"); config("samples", "1"); forceFresh = true;
      for (const char *mode : {"off", "auto"}) for (const char *reuse : {"0", "1"}) {
        config("upscaler", mode); config("waterIndirectReuse", reuse);
        for (int i = 0; i < 32; ++i) frame();
        const auto begin = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (int i = 0; i < 32; ++i) frame();
        const auto end = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        remixapi_ReconstructionDiagnosticsEXT reconstruction{};
        reconstruction.structSize = sizeof(reconstruction);
        ok(remixapi_GetReconstructionDiagnosticsEXT(&reconstruction));
        require(reconstruction.activeBackend == (std::string(mode) == "auto" ? 3u : 0u),
                "Water ray-count comparison did not select requested backend");
        std::printf("RAY_INTERVAL,%s,%s,%s,%lld,%lld\n", primary, mode, reuse,
                    static_cast<long long>(begin), static_cast<long long>(end));
        const auto color = readback(std::string(primary) + "-" + mode + "-reuse-" + reuse);
        if (reconstruction.activeBackend == 3)
          for (int c = 0; c < 3; ++c)
            require(std::abs(color[c] - reference[c]) < reference[c] * .1f + .001f,
                    "RR transparent material guides changed accumulated lighting energy");
      }
    }
    ok(api.Shutdown()); return 0;
  }
  for (const char *primary : {"raster", "ray"}) for (int scene = 0; scene < 5; ++scene) {
    config("primaryVisibility", primary);
    reverse = scene == 2;
    camera.view[0][0] = reverse ? -1 : 1;
    camera.view[2][2] = reverse ? -1 : 1;
    camera.view[3][2] = reverse ? 1 : 0; // Public camera arrays store columns.
    enclosure = {water, shore};
    if (scene == 1) enclosure.push_back(overlap);
    if (scene == 3) enclosure.push_back(indirectBlocker);
    if (scene == 4) enclosure = {ordinaryWater, shore, indirectBlocker};
    const std::string prefix = std::string(primary) + "-" + std::to_string(scene);
    config("samples", "16"); config("accumulate", "1");
    forceFresh = false;
    const auto reference = capture(prefix + "-reference", 32);
    const auto referenceRegion = region;
    remixapi_TracingStatisticsEXT tracing{}; tracing.structSize = sizeof(tracing);
    ok(remixapi_GetTracingStatisticsEXT(&tracing));
    require(tracing.accumulationCount >= 32, "Water reference failed to accumulate");
    forceFresh = true;
    config("samples", "1"); config("accumulate", "0");
    std::array<float, 3> averages[2]{}, regionMeans[2]{};
    double squaredError[2]{}, temporalVariance[2]{};
    for (unsigned reuse = 0; reuse < 2; ++reuse) {
      config("waterIndirectReuse", reuse ? "1" : "0");
      capture(prefix + (reuse ? "-reuse" : "-full"), 64);
      const auto resets = temporal().historyResets;
      std::vector<std::array<double, 3>> sums(region.size()), squares(region.size());
      for (int sample = 0; sample < 24; ++sample) {
        frame(); const auto value = readback(prefix + "-" + std::to_string(reuse) + "-" + std::to_string(sample));
        for (int c = 0; c < 3; ++c) averages[reuse][c] += value[c] / 24;
        for (size_t i = 0; i < region.size(); ++i) for (int c = 0; c < 3; ++c) {
          const double v = region[i][c], error = v - referenceRegion[i][c];
          squaredError[reuse] += error * error / (24 * region.size() * 3);
          regionMeans[reuse][c] += v / (24 * region.size());
          sums[i][c] += v / 24;
          squares[i][c] += v * v / 24;
        }
      }
      require(temporal().activeTaa && temporal().historyResets == resets, "Water sharing reset temporal history");
      for (size_t i = 0; i < region.size(); ++i) for (int c = 0; c < 3; ++c)
        temporalVariance[reuse] += std::max(0., squares[i][c] - sums[i][c] * sums[i][c]) / (region.size() * 3);
      std::printf("%s mode %u: region RMSE %.5f, temporal RMS %.5f\n", prefix.c_str(), reuse,
                  std::sqrt(squaredError[reuse]), std::sqrt(temporalVariance[reuse]));
    }
    for (int c = 0; c < 3; ++c) {
      require(std::abs(averages[1][c] - reference[c]) < std::max(.025f, std::abs(averages[0][c] - reference[c]) + .01f),
              "Water reuse increased reference error beyond the full temporal estimator");
      require(std::abs(regionMeans[1][c] - regionMeans[0][c]) < .015f,
              "Water reuse changed spatial mean energy/tint");
    }
    // Compare a broad region against the accumulated image: a tiny patch's
    // temporal mean alone confounds spatial noise and the existing TAA bias.
    require(std::sqrt(squaredError[1]) <= std::sqrt(squaredError[0]) * 1.2 + .002,
            "Water reuse increased image error excessively");
    require(std::sqrt(temporalVariance[1]) <= std::sqrt(temporalVariance[0]) * 1.2 + .002,
            "Water reuse increased temporal noise excessively");
    config("bounces", "0");
    const auto zero = capture(prefix + "-zero", 48);
    if (!reverse) require(zero[2] < .002f, "Zero bounces illuminated the water surface");
    else require(averages[1][2] > zero[2] + .05f, "Reverse view failed to illuminate the water sheet");
    config("bounces", "1");
  }
  ok(api.Shutdown());
  std::puts("Water indirect reconstruction, tint, overlap, reverse view and partial workgroups passed");
}
