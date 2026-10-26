#include "runtime/prepared.h"
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
void ok(remixapi_ErrorCode value) {
  require(!value, "Layered depth API failure");
}
} // namespace
int main(int argc, char **argv) {
  require(argc == 2, "Supply screenshot directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  remixapi_Interface api{};
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = startup.windowHeight = 128;
#endif
  ok(api.Startup(&startup));
  // These fixtures measure the unexposed lighting/display contract.
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
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
  config("upscaler", "off");
  config("denoiser", "off");
  config("debugView", "1");
  for (const char *key :
       {"taa", "sigma", "sharc", "sky", "localLights"})
    config(key, "0");
  config("bounces", "0");
  // A biome-colored cutout over an untinted base, separated by BetaRT's
  // 0.001-block grass overlay offset. The holes must still expose the base.
  const uint8_t texels[] = {255, 255, 255, 255, 255, 255, 255, 0,
                            255, 255, 255, 0,   255, 255, 255, 255};
  remixapi_TextureInfo texture{};
  texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  texture.hash = 10;
  texture.width = texture.height = 2;
  texture.depth = texture.mipLevels = 1;
  texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  texture.data = texels;
  texture.dataSize = sizeof(texels);
  remixapi_TextureHandle textureHandle{};
  ok(api.CreateTexture(&texture, &textureHandle));
  std::array<remixapi_MaterialHandle, 2> materials{};
  std::array<remixapi_MeshHandle, 2> meshes{};
  const auto texturePath = std::filesystem::path("0xa");
  for (unsigned layer = 0; layer < 2; ++layer) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant =
        layer ? remixapi_Float3D{0, 1, 0} : remixapi_Float3D{1, 0, 0};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = layer ? 4 : 7;
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo material{};
    material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    material.pNext = &opaque;
    material.hash = 20 + layer;
    material.albedoTexture = layer ? texturePath.c_str() : nullptr;
    ok(api.CreateMaterial(&material, &materials[layer]));
    remixapi_HardcodedVertex vertices[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      vertices[i].position[0] = i == 0 || i == 3 ? -1 : 1;
      vertices[i].position[1] = i < 2 ? -1 : 1;
      vertices[i].position[2] = layer ? -.001f : 0;
      vertices[i].texcoord[0] = i == 0 || i == 3 ? 0 : 1;
      vertices[i].texcoord[1] = i < 2 ? 0 : 1;
      vertices[i].color = 0xffffffff;
    }
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = materials[layer];
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = 30 + layer;
    mesh.surfaces_values = &surface;
    mesh.surfaces_count = 1;
    ok(api.CreateMesh(&mesh, &meshes[layer]));
  }
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  for (unsigned i = 0; i < 4; ++i)
    camera.view[i][i] = 1;
  const float nearPlane = .05f, farPlane = 512.f;
  camera.projection[0][0] = camera.projection[1][1] = 1;
  camera.projection[2][2] = farPlane / (farPlane - nearPlane);
  camera.projection[2][3] = 1;
  camera.projection[3][2] = -farPlane * nearPlane / (farPlane - nearPlane);
  auto capture = [&](float distance, unsigned pose, bool reverseOrder,
                     const char *primary) {
    config("primaryVisibility", primary);
    const float angle = .035f * float(pose), c = std::cos(angle),
                s = std::sin(angle);
    camera.view[0][0] = c;
    camera.view[0][2] = s;
    camera.view[2][0] = -s;
    camera.view[2][2] = c;
    camera.view[3][2] = float(pose) * .013f;
    auto frame = remix::test::preparedFrame([&] {
      ok(api.PumpEvents());
      ok(api.SetupCamera(&camera));
      for (unsigned order = 0; order < 2; ++order) {
        const unsigned layer = reverseOrder ? 1 - order : order;
        remixapi_InstanceIdentityEXT identity{};
        identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
        identity.instanceId = layer + 1;
        identity.rasterVisible = 1;
        remixapi_InstanceInfo instance{};
        instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        instance.pNext = &identity;
        instance.mesh = meshes[layer];
        instance.doubleSided = 1;
        instance.transform.matrix[0][0] = instance.transform.matrix[1][1] =
            distance * .65f;
        instance.transform.matrix[2][2] = 1;
        instance.transform.matrix[2][3] = distance;
        ok(api.DrawInstance(&instance));
      }
      ok(api.Present(nullptr));
    });
    frame();
    const std::string label =
        std::string(primary) + "-" + std::to_string(int(distance)) + "-" +
        std::to_string(pose) + (reverseOrder ? "-base-last" : "-overlay-last");
    const auto path = (directory / (label + ".png")).string();
    ok(remix::test::requestScreenshot(api, path.c_str(), frame));
    frame();
    frame();
    int width, height, channels;
    auto *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    require(pixels && width == 128 && height == 128,
            "Missing layered depth screenshot");
    std::vector<bool> mask;
    for (int y = 40; y < 88; ++y)
      for (int x = 40; x < 88; ++x) {
        const auto *p = pixels + (y * width + x) * 4;
        require(p[0] > 240 || p[1] > 240, "Layered face missing");
        mask.push_back(p[1] > p[0]);
      }
    stbi_image_free(pixels);
    return mask;
  };
  unsigned failures = 0;
  for (float distance : {8.f, 32.f, 64.f, 128.f})
    for (unsigned pose = 0; pose < 3; ++pose) {
      const auto reference = capture(distance, pose, false, "ray");
      for (bool reverseOrder : {false, true}) {
        const auto raster = capture(distance, pose, reverseOrder, "raster");
        unsigned wrong = 0, green = 0;
        for (size_t i = 0; i < raster.size(); ++i) {
          wrong += raster[i] != reference[i];
          green += reference[i];
        }
        require(green > reference.size() / 4 &&
                    green < 3 * reference.size() / 4,
                "Cutout reference is invalid");
        std::printf("distance=%g pose=%u baseLast=%u wrong=%u/%zu\n", distance,
                    pose, reverseOrder, wrong, raster.size());
        failures += wrong > reference.size() / 100;
      }
    }
  ok(api.Shutdown());
  require(!failures,
          "Raster primary lost close cutout layers compared with ray primary");
  std::puts("Layered cutout depth agrees with ray visibility across camera "
            "poses and draw orders");
}
