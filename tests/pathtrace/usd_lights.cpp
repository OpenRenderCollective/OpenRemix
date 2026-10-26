#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <remix/remix_native_ext.h>
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
  void ok(remixapi_ErrorCode result) { require(!result, "Environment test API failure"); }
} // namespace
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
  startup.windowWidth = 96;
  startup.windowHeight = 96;
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
  ok(api.SetConfigVariable("rtx.native.pathtrace.samples", "1"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "0"));
  require(api.SetConfigVariable("rtx.native.pathtrace.sky", "2") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid sky toggle accepted");
  ok(api.SetConfigVariable("rtx.native.vsync", "False"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.sharc", "0"));
  for (const auto *key : {"overrideLight", "taa", "sharc", "sigma", "accumulate"})
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", "off"));
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
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1,
                  remixapi_MaterialHandle mat) {
    remixapi_HardcodedVertex vertices[4]{};
    for (int i = 0; i < 4; ++i) {
      vertices[i].position[axis] = plane;
      vertices[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      vertices[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      vertices[i].color = 0xffffffff;
    }
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = mat;
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = next++;
    mesh.surfaces_values = &surface;
    mesh.surfaces_count = 1;
    remixapi_MeshHandle handle{};
    ok(api.CreateMesh(&mesh, &handle));
    return handle;
  };

  const auto receiver = quad(2, .8f, -2, 2, -2, 2, white);
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  for (int c = 0; c < 4; ++c)
    camera.view[c][c] = camera.projection[c][c] = 1;
  remixapi_InstanceInfo instance{};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.mesh = receiver;
  instance.doubleSided = true;
  for (int c = 0; c < 3; ++c)
    instance.transform.matrix[c][c] = 1;
  remixapi_LightInfoUSDEXT usd{};
  usd.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
  for (int c = 0; c < 3; ++c)
    usd.transform.matrix[c][c] = 1;
  usd.transform.matrix[2][2] = -1;
  usd.transform.matrix[2][3] = .1f;
  remixapi_Float3D red{1, .1f, .01f};
  float power = 4, radius = .2f, width = .4f, height = .4f, length = .4f;
  usd.pColor = &red;
  usd.pIntensity = &power;
  usd.pRadius = &radius;
  usd.pWidth = &width;
  usd.pHeight = &height;
  usd.pLength = &length;
  remixapi_LightInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.hash = 500;
  info.pNext = &usd;
  remixapi_LightHandle light{};
  bool geometry = true;
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    if (geometry)
      ok(api.DrawInstance(&instance));
    ok(api.DrawLightInstance(light));
    ok(api.Present(nullptr));
  });
  auto capture = [&](const std::string &name) {
    ok(api.CreateLight(&info, &light));
    frame();
    auto path = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, path.c_str(), frame));
    frame();
    frame();
    int w, h, n;
    auto image = stbi_load(path.c_str(), &w, &h, &n, 4);
    require(image, "GPU light readback missing");
    std::array<float, 3> mean{};
    for (int y = h / 2 - 8; y < h / 2 + 8; ++y)
      for (int x = w / 2 - 8; x < w / 2 + 8; ++x)
        for (int c = 0; c < 3; ++c)
          mean[c] += image[(y * w + x) * 4 + c] / (255.f * 256);
    stbi_image_free(image);
    std::printf("%s: %.6f %.6f %.6f\n", name.c_str(), mean[0], mean[1], mean[2]);
    std::fflush(stdout);
    ok(api.DestroyLight(light));
    return mean;
  };
  for (auto [name, type] : std::vector<std::pair<const char *, remixapi_StructType>>{
           {"sphere", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT},
           {"rectangle", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT},
           {"disk", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT},
           {"cylinder", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT},
           {"distant", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT},
           {"dome", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT}}) {
    usd.lightType = type;
    auto energy = capture(name);
    require(energy[0] > .03f && energy[0] > energy[1] * 1.4f,
            "Light type must illuminate receiver with authored color");
  }
  usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  float cone = .4f;
  usd.pConeAngleRadians = &cone;
  auto toward = capture("spot-toward");
  usd.transform.matrix[2][2] = 1;
  auto away = capture("spot-away");
  require(toward[0] > away[0] + .03f, "Spot direction must control falloff");
  usd.transform.matrix[2][2] = -1;
  usd.pConeAngleRadians = nullptr;
  usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT;
  width = height = .1f;
  auto small = capture("area-small");
  width = height = .8f;
  auto large = capture("area-large");
  require(large[0] > small[0] + .03f, "Area dimensions must change illumination");

  // A tiny RGBE image carries values greater than one to the sampled GPU image.
  auto skyFile = (directory / "colored-hdr.hdr").string();
  {
    std::ofstream file(skyFile, std::ios::binary);
    file << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 4; ++x) {
        uint8_t rgb[] = {uint8_t(x < 2 ? 128 : 0), uint8_t(x >= 2 ? 128 : 0), 0, 132};
        file.write((char *)rgb, 4);
      }
  }
  remixapi_LightInfoDomeEXT dome{};
  dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
  dome.colorTexture = skyFile.c_str();
  for (int c = 0; c < 3; ++c)
    dome.transform.matrix[c][c] = 1;
  usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
  usd.pNext = &dome;
  red = {1, 1, 1};
  power = .05f;
  geometry = false;
  auto greenSky = capture("hdr-dome-green");
  dome.transform.matrix[0][0] = dome.transform.matrix[2][2] = -1;
  auto redSky = capture("hdr-dome-rotated");
  require(greenSky[1] > .5f && greenSky[1] > greenSky[0] + .2f && redSky[0] > .5f &&
              redSky[0] > redSky[1] + .2f,
          "HDR dome upload and rotation");
  usd.pNext = nullptr;
  geometry = true;
  usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  power = 1;
  auto normalFile = (directory / "normal.dds").string();
  {
    uint32_t h[32] = {};
    h[0] = 0x20534444;
    h[1] = 124;
    h[2] = 0x100f;
    h[3] = h[4] = 1;
    h[5] = 4;
    h[7] = 1;
    h[19] = 32;
    h[20] = 0x41;
    h[22] = 32;
    h[23] = 255;
    h[24] = 0xff00;
    h[25] = 0xff0000;
    h[26] = 0xff000000;
    h[27] = 0x1000;
    std::ofstream file(normalFile, std::ios::binary);
    file.write((char *)h, sizeof(h));
    uint8_t pixel[] = {240, 128, 255, 255};
    file.write((char *)pixel, 4);
  }
  remixapi_MaterialNormalEncodingEXT encoding{};
  encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {1, 1, 1};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  opaque.roughnessConstant = 1;
  opaque.pNext = &encoding;
  remixapi_MaterialInfo materialInfo{};
  materialInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  materialInfo.hash = 900;
  materialInfo.pNext = &opaque;
  materialInfo.normalTexture = normalFile.c_str();
  remixapi_MaterialHandle normalMaterial{};
  ok(api.CreateMaterial(&materialInfo, &normalMaterial));
  instance.mesh = quad(2, .8f, -2, 2, -2, 2, normalMaterial);
  auto legacyNormal = capture("legacy-normal");
  encoding.encoding = 1;
  ok(api.CreateMaterial(&materialInfo, &normalMaterial));
  auto octNormal = capture("octahedral-normal");
  require(std::abs(legacyNormal[0] - octNormal[0]) > .01f,
          "Optional normal encoding must affect lighting on material update");
  ok(api.Shutdown());
  std::puts("Authored light GPU fixtures passed");
  return 0;
}
