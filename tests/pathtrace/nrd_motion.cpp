#include "runtime/prepared.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <string>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void ok(remixapi_ErrorCode code) { require(!code, "NRD motion API failure"); }
}

int main(int argc, char **argv) {
  require(argc == 2, "Supply screenshot directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = startup.windowHeight = 256;
#endif
  ok(api.Startup(&startup));
  // These fixtures measure the unexposed lighting/display contract.
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
  remixapi_DenoisingCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetDenoisingCapabilitiesEXT(&caps));
  if (!caps.reblurShSupported) { ok(api.Shutdown()); return 77; }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  // Captures are read right after the frame that writes them.
  ok(api.SetConfigVariable("rtx.native.asyncPresent", "0"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  config("upscaler", "off");
  config("denoiser", "reblur-sh");
  for (const auto *key : {"taa", "sigma", "sharc", "prism", "gamma22", "localLights"}) config(key, "0");
  config("bounces", "1");
  config("validation", "1");
  remixapi_MaterialInfoPbrEXT pbr{};
  pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
  pbr.enabled = 1;
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.pNext = &pbr;
  opaque.albedoConstant = {.5f, .5f, .5f};
  opaque.roughnessConstant = .7f;
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  remixapi_MaterialInfo material{};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = 1;
  remixapi_MaterialHandle materialHandle{};
  ok(api.CreateMaterial(&material, &materialHandle));
  remixapi_HardcodedVertex vertices[4]{};
  for (unsigned i = 0; i < 4; ++i) {
    vertices[i].position[0] = i == 0 || i == 3 ? -100.f : 100.f;
    vertices[i].position[1] = -1.5f;
    vertices[i].position[2] = i < 2 ? -100.f : 100.f;
    vertices[i].color = 0xffffffff;
  }
  uint32_t indices[] = {0, 2, 1, 0, 3, 2};
  remixapi_MeshInfoSurfaceTriangles surface{};
  surface.vertices_values = vertices; surface.vertices_count = 4;
  surface.indices_values = indices; surface.indices_count = 6;
  surface.material = materialHandle;
  remixapi_MeshInfo mesh{};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 2; mesh.surfaces_values = &surface; mesh.surfaces_count = 1;
  remixapi_MeshHandle meshHandle{};
  ok(api.CreateMesh(&mesh, &meshHandle));
  remixapi_InstanceIdentityEXT identity{};
  identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
  identity.instanceId = 1; identity.rasterVisible = 1;
  remixapi_InstanceInfo instance{};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.pNext = &identity; instance.mesh = meshHandle; instance.doubleSided = 1;
  for (int i = 0; i < 3; ++i) instance.transform.matrix[i][i] = 1;
  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1; environment.radiance = {.2f, .2f, .2f};
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &environment;
  camera.projection[0][0] = camera.projection[1][1] = 1;
  camera.projection[2][2] = 512.f / 511.95f;
  camera.projection[2][3] = 1;
  camera.projection[3][2] = -.05f * camera.projection[2][2];
  float handedness = 1;
  auto frame = remix::test::preparedFrame([&](float position, float pitch) {
    const float c = std::cos(pitch), s = std::sin(pitch);
    camera.view[0][0] = camera.view[3][3] = 1;
    camera.view[1][1] = c;
    camera.view[2][2] = c * handedness;
    camera.view[1][2] = -s * handedness;
    camera.view[2][1] = s;
    camera.view[3][1] = -position * s;
    camera.view[3][2] = -position * c * handedness;
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawInstance(&instance));
    ok(api.Present(nullptr));
  });
  unsigned failures = 0;
  for (bool rightHanded : {false, true}) for (const auto *primary : {"raster", "ray"}) {
    handedness = rightHanded ? -1.f : 1.f;
    camera.projection[2][2] = handedness * 512.f / 511.95f;
    camera.projection[2][3] = handedness;
    const std::string label = std::string(rightHanded ? "rh-" : "lh-") + primary;
    config("primaryVisibility", primary);
    for (unsigned i = 0; i < 24; ++i) frame(0, .3f);
    for (unsigned pose = 0; pose < 3; ++pose) {
      const auto path = (directory / (label + "-" + std::to_string(pose) + ".png")).string();
      for (unsigned i = 0; i < 24; ++i) {
        const unsigned step = pose * 24 + i;
        if (i == 22) ok(api.RequestPresentedScreenshot(path.c_str()));
        frame(.04f * float(step), .3f + .002f * float(step));
      }
      int width, height, channels;
      auto *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
      require(pixels && width == 256 && height == 256, "Missing NRD validation capture");
      // In NRD's MV tile, static geometry must agree with camera-derived
      // reprojection even while translating and rotating. Exclude labels,
      // sky, and viewport edges; red/green encode error in pixels.
      unsigned bad = 0, count = 0;
      double error = 0;
      for (int y = 30; y < 58; ++y) for (int x = 204; x < 244; ++x) {
        const auto *p = pixels + (y * width + x) * 4;
        error += (p[0] + p[1]) / 510.;
        bad += p[0] > 12 || p[1] > 12;
        ++count;
      }
      // The Units tile reconstructs camera-relative world position. Ground Y
      // is constant (-1.5), so its fractional green channel must stay uniform.
      // Stay below the jitter/rotator mini-viewports at the top of the tile.
      unsigned minGreen = 255, maxGreen = 0;
      for (int y = 36; y < 58; ++y) for (int x = 12; x < 52; ++x) {
        const unsigned green = pixels[(y * width + x) * 4 + 1];
        minGreen = std::min(minGreen, green); maxGreen = std::max(maxGreen, green);
      }
      stbi_image_free(pixels);
      std::printf("%s pose=%u motion disagreement=%u/%u mean=%g unitsY=%u..%u\n",
                  label.c_str(), pose, bad, count, error / count, minGreen, maxGreen);
      failures += bad > count / 100;
      failures += maxGreen - minGreen > 3 || minGreen < 160 || maxGreen > 210;
    }
  }
  ok(api.Shutdown());
  require(!failures, "NRD reconstructed camera motion disagrees with static ground guides");
  std::puts("NRD camera reconstruction agrees with motion guides in raster and ray visibility");
}
