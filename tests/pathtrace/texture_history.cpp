#include "runtime/prepared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <remix/remix_reconstruction_ext.h>
#include <string>
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
  void ok(remixapi_ErrorCode result) { require(!result, "Texture history API failure"); }
} // namespace
int main(int argc, char **argv) {
  require(argc == 2, "Supply capture directory");
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
  startup.windowWidth = startup.windowHeight = 512;
#endif
  ok(api.Startup(&startup));
  // These fixtures measure the unexposed lighting/display contract.
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "off"));
  remixapi_BackendCapabilitiesEXT capabilities{};
  capabilities.structSize = sizeof(capabilities);
  ok(remixapi_GetBackendCapabilitiesEXT(&capabilities));
  if (!(capabilities.supportedBackends & 2)) {
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  config("bounces", "0");
  config("denoiser", "off");
  config("sigma", "0");
  config("sharc", "0");
  config("sky", "0");
  config("localLights", "0");
  const char *ngxMode = std::getenv("REMIX_TEST_NGX");
  config("upscaler", ngxMode ? "dlss" : "fsr31");
  if (ngxMode)
    config("dlssRayReconstruction", std::string(ngxMode) == "rr" ? "1" : "0");
  std::array<uint8_t, 128 * 128 * 4> texels{};
  for (unsigned y = 0; y < 128; ++y)
    for (unsigned x = 0; x < 128; ++x) {
      const uint8_t value = ((x + y) & 1) ? 230 : 70;
      for (unsigned c = 0; c < 3; ++c)
        texels[(y * 128 + x) * 4 + c] = value;
      texels[(y * 128 + x) * 4 + 3] = 255;
    }
  remixapi_TextureInfo texture{};
  texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  texture.hash = 40;
  texture.width = texture.height = 128;
  texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  texture.data = texels.data();
  texture.dataSize = texels.size();
  remixapi_TextureHandle textureHandle{};
  ok(api.CreateTexture(&texture, &textureHandle));
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {1, 1, 1};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  remixapi_MaterialInfo material{};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = 41;
#if defined(_WIN32)
  material.albedoTexture = L"0x28";
#else
  material.albedoTexture = "0x28";
#endif
  remixapi_MaterialHandle materialHandle{};
  ok(api.CreateMaterial(&material, &materialHandle));
  remixapi_HardcodedVertex vertices[4]{};
  for (unsigned i = 0; i < 4; ++i) {
    vertices[i].texcoord[0] = i == 1 || i == 2 ? 1 : 0;
    vertices[i].texcoord[1] = i >= 2 ? 1 : 0;
    vertices[i].position[0] = vertices[i].texcoord[0] * 4 - 2;
    vertices[i].position[1] = vertices[i].texcoord[1] * 4 - 2;
    vertices[i].position[2] = 2;
    vertices[i].color = 0xffffffff;
  }
  uint32_t indices[] = {0, 1, 2, 0, 2, 3};
  remixapi_MeshInfoSurfaceTriangles surface{};
  surface.vertices_values = vertices;
  surface.vertices_count = 4;
  surface.indices_values = indices;
  surface.indices_count = 6;
  surface.material = materialHandle;
  remixapi_MeshInfo mesh{};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 42;
  mesh.surfaces_values = &surface;
  mesh.surfaces_count = 1;
  remixapi_MeshHandle meshHandle{};
  ok(api.CreateMesh(&mesh, &meshHandle));
  remixapi_InstanceIdentityEXT identity{};
  identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
  identity.instanceId = 1;
  identity.rasterVisible = 1;
  identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
  remixapi_InstanceInfo instance{};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.pNext = &identity;
  instance.mesh = meshHandle;
  instance.doubleSided = 1;
  // Rotate both the camera and its receiver, then move them together. The
  // image should be independent of their shared world-space translation.
  const float rotation[3][3] = {
      {.9776261f, 0, -.21035033f}, {-.017052714f, .9967086f, -.079254344f}, {.20965798f, .08106816f, .9744083f}};
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &epoch;
  camera.view[3][3] = 1;
  camera.projection[0][0] = camera.projection[1][1] = 1;
  camera.projection[2][2] = 100.f / 99.95f;
  camera.projection[2][3] = 1;
  camera.projection[3][2] = -5.f / 99.95f;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) {
      camera.view[c][r] = rotation[r][c];
      instance.transform.matrix[r][c] = rotation[c][r];
    }
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {rotation[2][0], rotation[2][1], rotation[2][2]};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 43;
  light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&light, &sun));
  unsigned sequence = 0;
  auto frame = remix::test::preparedFrame([&] {
    // Keep rendering past convergence caching, with negligible lighting change.
    distant.direction.x = rotation[2][0] + ((++sequence & 1) ? .0001f : 0);
    ok(api.UpdateLightDefinition(sun, &light));
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    ok(api.DrawInstance(&instance));
    ok(api.Present(nullptr));
  });
  if (ngxMode) {
    // NGX capabilities become available after the requested renderer prepares.
    frame();
    remixapi_ReconstructionDiagnosticsEXT d{};
    d.structSize = sizeof(d);
    ok(remixapi_GetReconstructionDiagnosticsEXT(&d));
    if (!(d.capabilities & (std::string(ngxMode) == "rr" ? 4 : 2))) {
      ok(api.Shutdown());
      return 77;
    }
  }
  double lastMean = 0;
  auto capture = [&](const std::string &name) {
    auto path = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, path.c_str(), frame));
    frame();
    frame();
    int w, h, n;
    auto bytes = stbi_load(path.c_str(), &w, &h, &n, 4);
    require(bytes, "Missing texture history capture");
    double gradient = 0, sum = 0, squares = 0;
    unsigned count = 0;
    for (int y = h / 4; y < h * 3 / 4; ++y)
      for (int x = w / 4; x < w * 3 / 4; ++x) {
        float v = bytes[(y * w + x) * 4] / 255.f;
        gradient += std::abs(v - bytes[(y * w + x + 1) * 4] / 255.f);
        sum += v;
        squares += v * v;
        ++count;
      }
    stbi_image_free(bytes);
    lastMean = sum / count;
    return std::array<double, 2>{gradient / count, squares / count - std::pow(sum / count, 2)};
  };
  auto stats = [&] {
    remixapi_DenoisingStatisticsEXT value{};
    value.structSize = sizeof(value);
    ok(remixapi_GetDenoisingStatisticsEXT(&value));
    return value;
  };
  for (const char *primary : {"raster", "ray"}) {
    config("primaryVisibility", primary);
    for (const char *quality : {"native-aa", "quality"}) {
      config(ngxMode ? "dlssQuality" : "fsrQuality", quality);
      std::array<double, 2> reference{};
      for (bool translated : {false, true}) {
        const float position[] = {translated ? 1023.247f : 0, translated ? 71.62f : 0, translated ? 1023.926f : 0};
        for (int r = 0; r < 3; ++r) {
          instance.transform.matrix[r][3] = position[r];
          camera.view[3][r] = 0;
          for (int c = 0; c < 3; ++c)
            camera.view[3][r] -= rotation[r][c] * position[c];
        }
        ++epoch.historyResetEpoch;
        const auto label = std::string(primary) + "-" + quality + (translated ? "-translated" : "-origin");
        for (int i = 0; i < 16; ++i)
          frame();
        require(!stats().activeTaa && stats().requestedTaa, "FSR must replace native TAA during texture history test");
        const auto resets = stats().historyResets;
        auto early = capture(label + "-early");
        for (int i = 0; i < 510; ++i)
          frame();
        auto late = capture(label + "-late");
        require(stats().historyResets == resets, "Stationary texture history was reset");
        std::printf("%s: gradient %.6f -> %.6f, variance %.6f -> %.6f\n", label.c_str(), early[0], late[0], early[1],
                    late[1]);
        if (!translated)
          reference = late;
        require(late[0] > .05 && late[1] > .01, "Texture contrast disappeared");
        // DLSS allows 10% RMS contrast variation (variance is squared RMS).
        // Keep the original, tighter FSR variance regression bound.
        require(late[0] > reference[0] * .9 && late[1] > reference[1] * (ngxMode ? .81 : .85),
                "Stationary FSR texture history depends on world translation");
        require(late[0] > early[0] * .95, "Stationary FSR progressively loses texture detail");
      }
    }
  }
  if (ngxMode) {
    remixapi_ReconstructionDiagnosticsEXT d{};
    d.structSize = sizeof(d);
    ok(remixapi_GetReconstructionDiagnosticsEXT(&d));
    require(d.activeBackend == (std::string(ngxMode) == "rr" ? 3u : 2u), "Texture test reconstruction unavailable");
    // Replace the streamed texture and move its instance without a camera cut.
    // Capture both colors after short settling periods to expose stale histories.
    for (unsigned tick = 0; tick < 4; ++tick) {
      for (unsigned i = 0; i < 128 * 128; ++i) {
        texels[i * 4] = tick & 1 ? 230 : 40;
        texels[i * 4 + 1] = 80;
        texels[i * 4 + 2] = 80;
      }
      ok(api.CreateTexture(&texture, &textureHandle));
      ok(api.CreateMaterial(&material, &materialHandle));
      instance.transform.matrix[0][3] += tick & 1 ? -.02f : .02f;
      for (int i = 0; i < 16; ++i)
        frame();
      capture("animated-moving-" + std::to_string(tick));
      require(tick & 1 ? lastMean > .6 : lastMean < .4, "Animated texture retained stale color");
    }
  }
  ok(api.Shutdown());
}
