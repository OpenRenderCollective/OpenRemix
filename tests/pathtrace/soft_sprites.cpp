// Sprite overlays over a traced scene (remixapi_InstanceInfoSpriteEXT with
// SOFT_DEPTH): the presentation pass captures the scene's depth after the
// tracer's composite and the overlay fades by its distance to it, for both
// primary-visibility modes and with or without reconstruction (the depth is
// taken at output resolution).
#include "runtime/prepared.h"
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
  void ok(remixapi_ErrorCode result) { require(!result, "Soft sprite API failure"); }
} // namespace

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
#ifndef _WIN32
  startup.windowWidth = 256;
  startup.windowHeight = 128;
#endif
  ok(api.Startup(&startup));
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
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  for (const char *key : {"sharc", "sigma", "taa", "bounces"})
    config(key, "0");
  config("denoiser", "off");
  config("upscaler", "off");
  const char *mode = std::getenv("REMIX_TEST_SOFT_RECONSTRUCTION");
  if (mode) {
    config("upscaler", mode);
    config("fsrQuality", "quality");
  }
  remixapi_CameraInfoParameterizedEXT parameters{};
  parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  parameters.right = {1, 0, 0};
  parameters.up = {0, 1, 0};
  parameters.forward = {0, 0, 1};
  parameters.aspect = 2;
  parameters.fovYInDegrees = 90;
  parameters.nearPlane = .1f;
  parameters.farPlane = 100;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &parameters;
  auto material = [&](uint64_t hash) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    opaque.useDrawCallAlphaState = 1;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = hash;
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  // A quad facing the camera at depth z, from x0 to x1 of the view's width
  // (-1 .. 1) and over its middle half in height.
  auto quad = [&](uint64_t hash, remixapi_MaterialHandle material, float z, float x0, float x1, uint32_t color,
                  float height = .5f) {
    remixapi_HardcodedVertex vertices[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      vertices[i].position[0] = (i == 0 || i == 3 ? x0 : x1) * 2 * z;
      vertices[i].position[1] = (i < 2 ? -height : height) * z;
      vertices[i].position[2] = z;
      vertices[i].color = color;
    }
    uint32_t indices[] = {0, 2, 1, 0, 3, 2};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = material;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle result{};
    ok(api.CreateMesh(&info, &result));
    return result;
  };
  const auto white = material(1);
  // The scene: a wall 3 units away (unlit: black, whatever the tone mapping).
  const auto wall = quad(2, white, 3, -1, 1, 0xffffffff, 1);
  // Green sprites 0.2 in front of it: soft over 0.4 (half shown), plain at
  // half alpha (the reference for it), soft over 0.05 (all shown), and soft
  // over 0.4 again where the scene's depth is the far plane (all shown).
  const auto half = quad(3, white, 2.8f, -.9f, -.6f, 0xff00ff00);
  const auto reference = quad(4, white, 2.8f, -.4f, -.1f, 0x8000ff00);
  const auto whole = quad(5, white, 2.8f, .1f, .4f, 0xff00ff00);
  const auto open = quad(6, white, 2.8f, .6f, .9f, 0xff00ff00);
  bool softSprites = true, wallBehindLast = true;
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    {
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 1;
      identity.rasterVisible = 1;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &identity;
      instance.mesh = wall;
      instance.doubleSided = 1;
      for (int i = 0; i < 3; ++i)
        instance.transform.matrix[i][i] = 1;
      // The wall ends before the last sprite unless wallBehindLast.
      instance.transform.matrix[0][0] = wallBehindLast ? 1.f : .5f;
      instance.transform.matrix[0][3] = wallBehindLast ? 0.f : -3.f;
      ok(api.DrawInstance(&instance));
    }
    const struct {
      remixapi_MeshHandle mesh;
      float softDistance;
    } sprites[] = {{half, .4f}, {reference, 0}, {whole, .05f}, {open, .4f}};
    for (const auto &entry : sprites) {
      remixapi_InstanceInfoBlendEXT blend{};
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      blend.alphaTestCompareOp = 7;
      blend.alphaBlendEnabled = 1;
      blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
      blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
      blend.writeMask = 15;
      blend.textureColorOperation = 3, blend.textureColorArg1Source = 1, blend.textureColorArg2Source = 2;
      blend.textureAlphaOperation = 3, blend.textureAlphaArg1Source = 1, blend.textureAlphaArg2Source = 2;
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.pNext = &blend;
      identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
      identity.rasterVisible = 1;
      remixapi_InstanceInfoSpriteEXT sprite{};
      sprite.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT;
      sprite.pNext = &identity;
      sprite.flags = REMIXAPI_SPRITE_SOFT_DEPTH_EXT;
      sprite.softDistance = entry.softDistance;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = entry.softDistance > 0 && softSprites ? static_cast<void *>(&sprite) : static_cast<void *>(&identity);
      instance.mesh = entry.mesh;
      instance.doubleSided = 1;
      for (int i = 0; i < 3; ++i)
        instance.transform.matrix[i][i] = 1;
      ok(api.DrawInstance(&instance));
    }
    ok(api.Present(nullptr));
  });
  auto capture = [&](const std::string &label) {
    for (unsigned i = 0; i < (mode ? 32u : 4u); ++i)
      frame();
    const auto file = (directory / (label + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, channels;
    auto *data = stbi_load(file.c_str(), &w, &h, &channels, 4);
    require(data && w == 256 && h == 128, "Soft sprite capture unavailable");
    // Green at the middle of each sprite (x = -.75, -.25, .25, .75 of the width).
    std::array<float, 4> result{};
    for (unsigned sample = 0; sample < 4; ++sample)
      for (int y = h / 2 - 4; y < h / 2 + 4; ++y)
        for (int x = int(sample * 64 + 28); x < int(sample * 64 + 36); ++x)
          result[sample] += data[(y * w + x) * 4 + 1] / (255.f * 64);
    stbi_image_free(data);
    if (mode) {
      remixapi_ReconstructionDiagnosticsEXT d{};
      d.structSize = sizeof(d);
      ok(remixapi_GetReconstructionDiagnosticsEXT(&d));
      require(d.activeBackend == 1u, "Soft sprite test reconstruction fell back");
    }
    std::printf("%s: half way %.3f, reference (alpha 1/2) %.3f, beyond its distance %.3f, last %.3f\n", label.c_str(),
                result[0], result[1], result[2], result[3]);
    return result;
  };
  for (const char *primary : {"ray", "raster"}) {
    config("primaryVisibility", primary);
    softSprites = wallBehindLast = true;
    auto green = capture(std::string(primary) + "-soft");
    require(green[1] > .2f && green[1] < .8f, "Reference sprite is not half transparent");
    require(std::abs(green[0] - green[1]) < .03f, "Soft sprite half way into its distance is not half transparent");
    require(green[2] > .97f, "Soft sprite beyond its distance is not opaque");
    require(std::abs(green[3] - green[1]) < .03f, "Soft sprite in front of the wall (last) is not half transparent");
    wallBehindLast = false;
    green = capture(std::string(primary) + "-open");
    require(std::abs(green[0] - green[1]) < .03f && green[3] > .97f, "Soft sprite in front of nothing is not opaque");
    softSprites = false;
    green = capture(std::string(primary) + "-hard");
    require(green[0] > .97f && green[2] > .97f && green[3] > .97f, "Sprites without the extension are not opaque");
  }
  ok(api.Shutdown());
  std::puts("Soft sprite overlays over a traced scene passed");
  return 0;
}
