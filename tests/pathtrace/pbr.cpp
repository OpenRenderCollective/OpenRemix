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
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>
void require(bool v, const char *why) {
  if (!v) {
    std::fprintf(stderr, "%s\n", why);
    std::exit(1);
  }
}
void ok(remixapi_ErrorCode code) {
  if (code) {
    std::fprintf(stderr, "PBR API error %x\n", unsigned(code));
    std::exit(1);
  }
}
int main(int argc, char **argv) {
  require(argc == 2, "Supply PBR screenshot directory");
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
  startup.windowWidth = startup.windowHeight = 64;
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
    std::printf("PBR unavailable: %s\n", caps.unavailableReason);
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable(
        (std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  for (const char *key : {"taa", "sharc", "sigma"})
    config(key, "0");
  config("denoiser", "off");
  config("samples", "16");
  config("bounces", "1");
  config("accumulate", "1");
  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1;
  environment.radiance = {.15f, .25f, .35f};
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &environment;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {.6f, 0, .8f};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 1;
  light.radiance = {1, 1, 1};
  remixapi_LightHandle sun;
  ok(api.CreateLight(&light, &sun));
  auto texture = [&](uint64_t id, std::array<uint8_t, 4> pixel) {
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = id;
    info.width = info.height = info.depth = info.mipLevels = 1;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    info.data = pixel.data();
    info.dataSize = 4;
    remixapi_TextureHandle handle;
    ok(api.CreateTexture(&info, &handle));
  };
  texture(10, {102, 0, 0, 255});
  texture(11, {204, 0, 0, 255});
  texture(12, {128, 128, 255, 255});
  texture(13, {255, 0, 0, 255});
  auto path = [](const char *name) { return std::filesystem::path(name); };
  const auto roughness = path("0xa"), metallic = path("0xb"),
             normal = path("0xc"), emissive = path("0xd");
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {.7f, .3f, .1f};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  opaque.roughnessConstant = .4f;
  opaque.metallicConstant = .8f;
  remixapi_MaterialInfo material{};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = 20;
  remixapi_MaterialHandle mat;
  ok(api.CreateMaterial(&material, &mat));
  remixapi_HardcodedVertex vertices[4]{};
  for (unsigned i = 0; i < 4; ++i) {
    vertices[i].position[0] = (i == 0 || i == 3) ? -2 : 2;
    vertices[i].position[1] = i < 2 ? -2 : 2;
    vertices[i].position[2] = .5f;
    vertices[i].texcoord[0] = (i == 0 || i == 3) ? 0 : 1;
    vertices[i].texcoord[1] = i < 2 ? 0 : 1;
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
  mesh.hash = 30;
  mesh.surfaces_values = &surface;
  mesh.surfaces_count = 1;
  remixapi_MeshHandle geometry;
  ok(api.CreateMesh(&mesh, &geometry));
  remixapi_InstanceInfo instance{};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.mesh = geometry;
  instance.doubleSided = 1;
  for (int i = 0; i < 3; ++i)
    instance.transform.matrix[i][i] = 1;
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    ok(api.DrawInstance(&instance));
    ok(api.Present(nullptr));
  });
  auto capture = [&](const char *name) {
    ok(api.CreateMaterial(&material, &mat));
    for (int i = 0; i < 16; ++i)
      frame();
    const auto file = (directory / (std::string(name) + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, c;
    auto *bytes = stbi_load(file.c_str(), &w, &h, &c, 4);
    require(bytes, "Missing PBR screenshot");
    std::array<double, 3> result{};
    int count = 0;
    for (int y = h / 4; y < 3 * h / 4; ++y)
      for (int x = w / 4; x < 3 * w / 4; ++x) {
        ++count;
        for (int channel = 0; channel < 3; ++channel) {
          double v = bytes[(y * w + x) * 4 + channel] / 255.;
          result[channel] += std::pow(v, 2.2);
        }
      }
    stbi_image_free(bytes);
    for (auto &v : result)
      v /= count;
    std::printf("%s %.5f %.5f %.5f\n", name, result[0], result[1], result[2]);
    return result;
  };
  auto constants = capture("constants");
  opaque.roughnessConstant = .95f;
  opaque.metallicConstant = 0; // Maps replace these constants.
  opaque.roughnessTexture = roughness.c_str();
  opaque.metallicTexture = metallic.c_str();
  auto maps = capture("maps");
  for (int c = 0; c < 3; ++c)
    require(std::abs(constants[c] - maps[c]) < .012,
            "PBR maps differ from linear constants");
  material.normalTexture = normal.c_str();
  auto flat = capture("flat-normal");
  for (int c = 0; c < 3; ++c)
    require(std::abs(maps[c] - flat[c]) < .012,
            "Flat normal changed material frame");
  texture(12, {218, 128, 218, 255});
  auto tilted = capture("tilted-normal");
  require(std::abs(tilted[0] - flat[0]) > .015,
          "Normal map has no shading effect");
  config("primaryVisibility", "ray");
  auto ray = capture("ray-normal");
  for (int c = 0; c < 3; ++c)
    require(std::abs(ray[c] - tilted[c]) < .02,
            "Raster/ray PBR evaluation disagrees");
  config("primaryVisibility", "raster");
  material.normalTexture = nullptr;
  material.emissiveTexture = emissive.c_str();
  material.emissiveColorConstant = {1, 1, 1};
  material.emissiveIntensity = .1f;
  auto emission = capture("emission");
  require(emission[0] > maps[0] + .075 &&
              std::abs(emission[1] - maps[1]) < .015,
          "Emissive map not added in linear space");
  material.emissiveIntensity = 0;
  opaque.roughnessTexture = nullptr;
  opaque.metallicTexture = nullptr;
  opaque.roughnessConstant = 0;
  opaque.metallicConstant = 1;
  auto smooth = capture("smooth-metal");
  require(smooth[0] > .06 && smooth[2] > .01,
          "Smooth metal lost reflected sky");
  config("accumulate", "0");
  config("denoiser", "reblur-sh");
  config("taa", "1");
  auto temporal = capture("temporal-metal");
  require(temporal[0] > .03, "Temporal composition lost specular reflection");
  ok(api.Shutdown());
  std::puts("PBR maps, normals, reflected sky, emission and temporal "
            "composition passed");
}
