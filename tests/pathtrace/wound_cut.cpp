// remixapi_InstanceInfoWoundCutEXT and remixapi_MaterialGradientMapEXT in the
// path tracer: a wound cut holes the surface for camera rays (traced and
// raster primary visibility) and for shadow rays, and a gradient-mapped,
// self-illuminated quad shows its composite (the skin row, the sprite's
// blood, the blood a wound's W adds and the burn) and fades out below
// opacity 1.
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <string>
#include <thread>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

#ifdef _WIN32
#define PATH(text) L##text
#else
#define PATH(text) text
#endif

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "Wound cut test failed: %s\n", message);
      std::exit(1);
    }
  }
  void ok(remixapi_ErrorCode result) { require(!result, "API failure"); }
  struct Color {
    float r = 0, g = 0, b = 0;
  };
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
  ok(api.SetConfigVariable("rtx.native.asyncPresent", "0"));
  ok(api.SetConfigVariable("rtx.native.lowLatency", "0"));
  // Windows StartupInfo names a host HWND, not dimensions. Resize the owned
  // SDL window just as the other cross-platform pixel fixtures do.
  int windowCount = 0;
  SDL_Window **windows = SDL_GetWindows(&windowCount);
  require(windows && windowCount > 0, "Owned test window");
  SDL_SetWindowSize(windows[0], 256, 128);
  SDL_free(windows);
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
  auto texture = [&](uint64_t hash, std::vector<uint8_t> texel, remixapi_Format format) {
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = hash;
    info.width = info.height = info.depth = info.mipLevels = 1;
    info.format = format;
    info.data = texel.data();
    info.dataSize = texel.size();
    remixapi_TextureHandle handle{};
    ok(api.CreateTexture(&info, &handle));
  };
  texture(0x81, {255, 255, 255, 255}, REMIXAPI_FORMAT_R8G8B8A8_UNORM); // cut: white
  texture(0x82, {0, 0, 255, 255}, REMIXAPI_FORMAT_R8G8B8A8_UNORM);     // B: skin, luminance 1, no masks
  texture(0x83, {0, 255, 0, 255}, REMIXAPI_FORMAT_R8G8B8A8_SRGB);      // the gradient: green
  texture(0x84, {0, 255, 255, 255}, REMIXAPI_FORMAT_R8G8B8A8_UNORM);   // B: the same, all blood
  texture(0x85, {255, 0, 0, 255}, REMIXAPI_FORMAT_R8G8B8A8_SRGB);      // the burn: red, opaque
  // emission: a constant colour (no texture); selfLit: its albedo emits.
  auto material = [&](uint64_t hash, Color albedo, Color emission, const void *next = nullptr,
                      remixapi_Path texture = nullptr, float opacity = 1) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = const_cast<void *>(next);
    opaque.albedoConstant = {albedo.r, albedo.g, albedo.b};
    opaque.opacityConstant = opacity;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = hash;
    info.albedoTexture = texture;
    info.emissiveIntensity = 1;
    info.emissiveColorConstant = {emission.r, emission.g, emission.b};
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  auto quad = [&](uint64_t hash, remixapi_MaterialHandle material, float x0, float x1, float y0, float y1,
                  float z) {
    remixapi_HardcodedVertex vertices[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      vertices[i].position[0] = i == 0 || i == 3 ? x0 : x1;
      vertices[i].position[1] = i < 2 ? y0 : y1;
      vertices[i].position[2] = z;
      vertices[i].color = 0xffffffff;
      vertices[i].texcoord[0] = i == 0 || i == 3 ? 0.f : 1.f;
      vertices[i].texcoord[1] = i < 2 ? 1.f : 0.f;
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
  // The camera scene: an emissive red quad in front of an emissive green one.
  const auto green = quad(1, material(11, {}, {0, 1, 0}), -8, 8, -4, 4, 3);
  const auto red = quad(2, material(12, {}, {1, 0, 0}), -4, 4, -2, 2, 2);
  // The shadow scene: a white wall lit by a sun travelling (1, 0, 1), and an
  // occluder at the left whose shadow the camera sees beside it.
  const auto wall = quad(3, material(13, {1, 1, 1}, {}), -8, 8, -4, 4, 3);
  const auto occluder = quad(4, material(14, {1, 1, 1}, {}), -2, -1, -1, 1, 2.5f);
  // The gradient scene: a self-illuminated gradient-mapped quad.
  remixapi_MaterialGradientMapEXT gradient{};
  gradient.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_GRADIENT_MAP_EXT;
  gradient.gradientTexture = PATH("0x83");
  gradient.bloodColor = {.5f, .5f, .5f};
  gradient.bloodRange[0] = .1f;
  gradient.bloodRange[1] = .9f;
  remixapi_MaterialInfoSelfIlluminationEXT self{};
  self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
  self.pNext = &gradient;
  self.mask = REMIXAPI_SELF_ILLUMINATION_MASK_NONE_EXT;
  self.maskScale = 1;
  auto infected = [&](remixapi_Path albedo, float opacity = 1) {
    return material(15, {1, 1, 1}, {1, 1, 1}, &self, albedo, opacity);
  };
  const auto body = quad(5, infected(PATH("0x82")), -4, 4, -2, 2, 2);
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {.70710678f, 0, .70710678f};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 6;
  light.radiance = {3, 3, 3};
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&light, &sun));

  remixapi_InstanceInfoWoundCutEXT wound{};
  wound.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_WOUND_CUT_EXT;
  wound.cutTexture = PATH("0x81");
  // The first ellipsoid: q at every vertex (e.z 0); the second far away.
  wound.ellipsoids[0].axisX = {.1f, 0, 0};
  wound.ellipsoids[0].axisY = {0, .1f, 0};
  wound.ellipsoids[1].center = {1e6f, 1e6f, 1e6f};
  wound.ellipsoids[1].axisX = {1, 0, 0};
  wound.ellipsoids[1].axisY = {0, 1, 0};
  wound.ellipsoids[1].axisZ = {0, 0, 1};
  wound.mappingScale = 7;
  for (int i = 0; i < 4; ++i)
    wound.projectionScale[i] = wound.projectionBias[i] = .5f;
  // Both halves sample the cut texture's centre: W 1.
  wound.cutScaleBias[0][2] = wound.cutScaleBias[0][3] = wound.cutScaleBias[1][2] = wound.cutScaleBias[1][3] = .5f;
  wound.threshold = .95f;
  int scene = 0;
  bool wounded = false;
  auto draw = [&](remixapi_MeshHandle mesh, uint64_t id, bool cut) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = cut ? &wound : nullptr;
    identity.instanceId = id;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &identity;
    instance.mesh = mesh;
    instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i)
      instance.transform.matrix[i][i] = 1;
    ok(api.DrawInstance(&instance));
  };
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    if (scene == 0) {
      draw(green, 1, false);
      draw(red, 2, wounded);
    } else if (scene == 1) {
      ok(api.DrawLightInstance(sun));
      draw(wall, 3, false);
      draw(occluder, 4, wounded);
    } else
      draw(body, 5, wounded);
    ok(api.Present(nullptr));
  });
  // The mean colour of an 8x8 block centred at (x, y), once the screenshot is written.
  auto capture = [&](const std::string &label, int x, int y) {
    for (unsigned i = 0; i < 4; ++i)
      frame();
    const auto file = (directory / (label + ".png")).string();
    std::filesystem::remove(file);
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    int w = 0, h = 0, channels = 0;
    unsigned char *data = nullptr;
    for (unsigned i = 0; i < 200 && !data; ++i) {
      frame();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      data = std::filesystem::exists(file) ? stbi_load(file.c_str(), &w, &h, &channels, 4) : nullptr;
    }
    require(data && w == 256 && h == 128, "Screenshot unavailable");
    Color c;
    for (int v = y - 4; v < y + 4; ++v)
      for (int u = x - 4; u < x + 4; ++u) {
        c.r += data[(v * w + u) * 4] / (255.f * 64);
        c.g += data[(v * w + u) * 4 + 1] / (255.f * 64);
        c.b += data[(v * w + u) * 4 + 2] / (255.f * 64);
      }
    stbi_image_free(data);
    std::printf("%s: %.3f %.3f %.3f\n", label.c_str(), c.r, c.g, c.b);
    return c;
  };
  for (const char *primary : {"ray", "raster"}) {
    config("primaryVisibility", primary);
    const std::string mode = primary;
    scene = 0;
    wounded = false;
    auto c = capture(mode + "-whole", 128, 64);
    require(c.r > c.g + .3f, "The uncut quad is not in front");
    wounded = true;
    c = capture(mode + "-cut", 128, 64);
    require(c.g > c.r + .3f, "The camera saw a cut surface");
    wound.threshold = 1;
    c = capture(mode + "-threshold", 128, 64);
    require(c.r > c.g + .3f, "W at the threshold cut");
    wound.threshold = .95f;
    // Shadows: the occluder's shadow beside it, gone with the occluder cut.
    scene = 1;
    wounded = false;
    const auto shadowed = capture(mode + "-shadow", 113, 64), lit = capture(mode + "-lit", 200, 64);
    require(shadowed.r < .5f * lit.r, "No shadow beside the occluder");
    wounded = true;
    c = capture(mode + "-shadow-cut", 113, 64);
    require(std::abs(c.r - lit.r) < .05f, "A cut occluder still cast its shadow");
    // The gradient map: the skin row, then the sprite's blood, then a
    // wound's (W 1 over the blood range, under the threshold).
    scene = 2;
    wounded = false;
    infected(PATH("0x82"));
    const auto skin = capture(mode + "-gradient", 128, 64);
    require(skin.g > skin.r + .3f && skin.g > skin.b + .3f, "The gradient's skin row");
    infected(PATH("0x84"));
    c = capture(mode + "-blood", 128, 64);
    require(c.g < .9f * skin.g && c.g > .2f * skin.g, "The sprite's blood");
    infected(PATH("0x82"));
    wounded = true;
    wound.threshold = 1;
    const auto bleeding = capture(mode + "-wound-blood", 128, 64);
    require(std::abs(bleeding.g - c.g) < .05f, "A wound's blood");
    wound.threshold = .95f;
    wounded = false;
    // The burn (red, opaque) at strength 1/2 over skin: half way from the
    // green composite to red.
    gradient.burnTexture = PATH("0x85");
    gradient.burnStrength = .5f;
    infected(PATH("0x82"));
    c = capture(mode + "-burn", 128, 64);
    require(c.r > c.b + .3f && std::abs(c.r - c.g) < .05f, "The burn");
    gradient.burnStrength = 1;
    infected(PATH("0x82"));
    c = capture(mode + "-burnt", 128, 64);
    require(c.r > c.g + .3f && c.r > c.b + .3f, "A full burn");
    gradient.burnTexture = nullptr;
    gradient.burnStrength = 0;
    // Fading out (opacity 1/2) over the empty background: blended as any
    // opaque material below opacity 1, dimmer than the opaque quad.
    infected(PATH("0x82"), .5f);
    c = capture(mode + "-faded", 128, 64);
    require(c.g < .9f * skin.g && c.g > .2f * skin.g && c.g > c.r + .2f, "A fading gradient map");
    infected(PATH("0x82"));
  }
  ok(api.DestroyLight(sun));
  ok(api.Shutdown());
  std::puts("Wound cuts and gradient maps in the path tracer passed");
  return 0;
}
