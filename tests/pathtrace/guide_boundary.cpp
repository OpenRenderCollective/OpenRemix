#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <string>
#include <vector>
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
void ok(remixapi_ErrorCode code) {
  require(code == REMIXAPI_ERROR_CODE_SUCCESS, "Guide boundary API failure");
}
using Image = std::vector<std::array<float, 3>>;
} // namespace
int main(int argc, char **argv) {
  require(argc == 2, "Supply capture directory");
  auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = 640;
  startup.windowHeight = 512;
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
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable(
        (std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  config("sharc", "0");
  config("sky", "0");
  config("sigma", "0");
  config("localRange", "16");
  config("localIntensity", "1");
  uint64_t next = 1;
  auto material = [&](remixapi_Float3D color, float alpha) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = next++;
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  auto white = material({.8f, .8f, .8f}, 1), black = material({0, 0, 0}, 1);
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1,
                  remixapi_MaterialHandle material) {
    remixapi_HardcodedVertex v[4]{};
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 4; ++i) {
      v[i].position[axis] = plane;
      v[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      v[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      v[i].color = 0xffffffff;
    }
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = v;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = material;
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = next++;
    mesh.surfaces_values = &surface;
    mesh.surfaces_count = 1;
    remixapi_MeshHandle result{};
    ok(api.CreateMesh(&mesh, &result));
    return result;
  };
  // Two parallel receivers share a tangential guide cell. The front plane
  // lies exactly on a cell boundary. The partition makes the many bright
  // sources useful only on the rear plane, so a wrong cell matters visibly.
  auto receiver = quad(2, 192.f, 191.f, 193.f, 63.f, 67.f, white);
  std::vector<remixapi_MeshHandle> meshes;
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = .04f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  std::array<remixapi_LightHandle, 17> lights{};
  for (auto &handle : lights) {
    light.hash = next++;
    ok(api.CreateLight(&light, &handle));
  }
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, -1, 0};
  remixapi_LightInfo zeroSun{};
  zeroSun.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  zeroSun.pNext = &distant;
  zeroSun.hash = next++;
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&zeroSun, &sun));
  unsigned renderSequence = 0;
  auto frame = remix::test::preparedFrame([&] {
    // Force fresh frames with an unlit sun; the camera and torch scene stay
    // fixed.
    distant.direction.x = (++renderSequence & 1) ? .001f : 0;
    ok(api.UpdateLightDefinition(sun, &zeroSun));
    ok(api.DrawLightInstance(sun));
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    for (auto handle : lights)
      ok(api.DrawLightInstance(handle));
    for (auto mesh : meshes) {
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = reinterpret_cast<uintptr_t>(mesh);
      identity.rasterVisible = 1;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      remixapi_InstanceInfoBlendEXT blend{};
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      blend.pNext = &identity;
      blend.alphaBlendEnabled = 0;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &blend;
      instance.mesh = mesh;
      instance.doubleSided = 1;
      for (int i = 0; i < 3; ++i)
        instance.transform.matrix[i][i] = 1;
      ok(api.DrawInstance(&instance));
    }
    ok(api.Present(nullptr));
  });
  auto capture = [&](const std::string &name, int radius = 12) {
    auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, n;
    auto data = stbi_load(file.c_str(), &w, &h, &n, 4);
    require(data, "Torch capture unavailable");
    Image result;
    float depth = 192.f + camera.view[3][2];
    int px = int(
        (1 + (192.5f + camera.view[3][0]) * camera.projection[0][0] / depth) *
        .5f * w);
    for (int y = h / 2 - radius; y < h / 2 + radius; ++y)
      for (int x = px - radius; x < px + radius; ++x) {
        std::array<float, 3> pixel{};
        for (int c = 0; c < 3; ++c) {
          float v = data[(y * w + x) * 4 + c] / 255.f;
          pixel[c] = std::pow(v, 2.2f);
        }
        result.push_back(pixel);
      }
    stbi_image_free(data);
    return result;
  };

  auto rear = quad(2, 193.f, 193.f, 197.f, 63.f, 67.f, white);
  auto divider = quad(0, 193.05f, 63.f, 67.f, 189.f, 193.1f, black);
  meshes = {receiver, rear, divider};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      camera.projection[i][j] = 0;
  camera.projection[0][0] = 1.6f;
  camera.projection[1][1] = 2.f;
  camera.projection[2][2] = 100.f / 99.95f;
  camera.projection[2][3] = 1.f;
  camera.projection[3][2] = -5.f / 99.95f;
  camera.view[3][0] = -193.f;
  camera.view[3][1] = -65.f;
  for (unsigned i = 0; i < lights.size(); ++i) {
    sphere.position = i == 0 ? remixapi_Float3D{192.4f, 65.f, 190.8f}
                             : remixapi_Float3D{193.6f, 65.f, 190.8f};
    sphere.radius = .08f;
    light.radiance = {20, 10, 4};
    ok(api.UpdateLightDefinition(lights[i], &light));
  }
  config("bounces", "0");
  config("denoiser", "off");
  config("taa", "1");
  config("localSamples", "1");
  config("localGuiding", "1");
  config("accumulate", "0");
  auto csv = std::fopen((directory / "metrics.csv").c_str(), "w");
  require(csv, "Cannot open guide boundary metrics");
  std::fputs("primary,frame,z,mean\n", csv);
  for (const char *primary : {"raster", "ray"}) {
    config("primaryVisibility", primary);
    ++epoch.historyResetEpoch;
    camera.view[3][2] = -188.f;
    for (int f = 0; f < 128; ++f)
      frame();
    remixapi_DenoisingStatisticsEXT before{};
    before.structSize = sizeof(before);
    ok(remixapi_GetDenoisingStatisticsEXT(&before));
    std::vector<double> means;
    // Pure camera translation with fixed light definitions and no GI or NRD.
    // Track a fixed world patch; changes in projected texture/footprint cannot
    // account for broad brightness pulses on this uniform Lambertian plane.
    for (int f = 0; f < 64; ++f) {
      camera.view[3][2] =
          -188.f - .125f * (1 - std::cos(float(f) * 3.14159265f / 16));
      auto img = capture(std::string(primary) + "-" + std::to_string(f));
      double mean = 0;
      for (auto p : img)
        mean += .2126 * p[0] + .7152 * p[1] + .0722 * p[2];
      mean /= img.size();
      std::fprintf(csv, "%s,%d,%.8f,%.9f\n", primary, f, camera.view[3][2],
                   mean);
      std::fflush(csv);
      means.push_back(mean);
    }
    remixapi_DenoisingStatisticsEXT after{};
    after.structSize = sizeof(after);
    ok(remixapi_GetDenoisingStatisticsEXT(&after));
    remixapi_LocalLightGuidingStatisticsEXT guide{};
    guide.structSize = sizeof(guide);
    ok(remixapi_GetLocalLightGuidingStatisticsEXT(&guide));
    require(after.activeTaa && !after.activeDenoiser &&
                after.historyResets == before.historyResets && guide.active &&
                guide.sampledGuided > 0,
            "Translation did not retain TAA and world-cell guidance");
    double average = 0;
    for (auto mean : means)
      average += mean / means.size();
    const auto extrema = std::minmax_element(means.begin(), means.end());
    const double span = *extrema.second - *extrema.first;
    std::printf("%s: mean %.6f, brightness span %.2f%%\n", primary, average,
                100 * span / average);
    require(average > .025 && average < .07,
            "Guide boundary receiver lost illumination");
    require(span < .15 * average,
            "Camera translation caused broad torch brightness pulses");

    // A distant receiver supplies only a handful of base training samples per
    // world cell. A one-frame guide alternately enables itself and then loses
    // the four observations needed to stay enabled. The physical camera,
    // receiver, blocker and all torch definitions remain exactly stationary.
    camera.view[3][2] = -160.f;
    for (int f = 0; f < 128; ++f)
      frame();
    ok(remixapi_GetDenoisingStatisticsEXT(&before));
    ok(remixapi_GetLocalLightGuidingStatisticsEXT(&guide));
    const auto guideFrames = guide.renderedFrames;
    means.clear();
    for (int f = 0; f < 160; ++f) {
      auto img =
          capture(std::string(primary) + "-stationary-" + std::to_string(f), 4);
      double mean = 0;
      for (auto p : img)
        mean += .2126 * p[0] + .7152 * p[1] + .0722 * p[2];
      mean /= img.size();
      means.push_back(mean);
      std::fprintf(csv, "%s-stationary,%d,%.8f,%.9f\n", primary, f,
                   camera.view[3][2], mean);
    }
    ok(remixapi_GetDenoisingStatisticsEXT(&after));
    ok(remixapi_GetLocalLightGuidingStatisticsEXT(&guide));
    require(after.historyResets == before.historyResets &&
                guide.renderedFrames >= guideFrames + 320,
            "Stationary guide test reset history or reused a cached frame");
    double stationaryMean = 0;
    for (auto mean : means)
      stationaryMean += mean / means.size();
    const auto stationaryExtrema =
        std::minmax_element(means.begin(), means.end());
    const double stationarySpan =
        *stationaryExtrema.second - *stationaryExtrema.first;
    std::printf("%s stationary: mean %.6f, brightness span %.2f%%\n", primary,
                stationaryMean, 100 * stationarySpan / stationaryMean);
    require(stationaryMean > .025 && stationaryMean < .07,
            "Sparse guide feedback lost receiver illumination");
    require(
        stationarySpan < .35 * stationaryMean,
        "Sparse guide feedback caused stationary lighting patches to switch");
  }
  std::fclose(csv);
  ok(api.Shutdown());
  std::puts("Torch guidance stays stable under translation and sparse "
            "stationary feedback");
}
