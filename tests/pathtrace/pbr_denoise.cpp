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
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
void require(bool value, const char *why) {
  if (!value) {
    std::fprintf(stderr, "%s\n", why);
    std::exit(1);
  }
}
void ok(remixapi_ErrorCode code) {
  if (code) {
    std::fprintf(stderr, "PBR denoising API error %x\n", unsigned(code));
    std::exit(1);
  }
}
using Image = std::vector<std::array<double, 3>>;
} // namespace
int main(int argc, char **argv) {
  require(argc == 2, "Supply capture directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  const bool rrTest = std::getenv("REMIX_TEST_RR") != nullptr;
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = startup.windowHeight = rrTest ? 256 : 96;
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
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  for (const auto *key : {"taa", "sigma", "sharc", "adaptiveLocalSampling"})
    config(key, "0");
  if (rrTest) {
    config("dlssQuality", "native-aa");
    config("primaryVisibility", std::getenv("REMIX_TEST_RAY_PRIMARY") ? "ray" : "raster");
  }
  config("bounces", "1");
  config("localSamples", "1");
  config("localDenoiser", "1");
  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1;
  environment.radiance = {.025f, .04f, .065f};
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.pNext = &environment;
  epoch.worldEpoch = 1;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  if (rrTest) {
    for (int i=0;i<4;++i) for (int j=0;j<4;++j) camera.projection[i][j]=0;
    camera.projection[0][0]=camera.projection[1][1]=1;
    camera.projection[2][2]=100.f/99.95f; camera.projection[2][3]=1;
    camera.projection[3][2]=-5.f/99.95f;
  }
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {.55f, .25f, .1f};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  remixapi_MaterialInfo material{};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = 1;
  remixapi_MaterialHandle mat{};
  opaque.roughnessConstant = .65f;
  ok(api.CreateMaterial(&material, &mat));
  remixapi_HardcodedVertex vertices[4]{};
  for (int i = 0; i < 4; ++i) {
    vertices[i].position[0] = i == 0 || i == 3 ? -2 : 2;
    vertices[i].position[1] = i < 2 ? -2 : 2;
    vertices[i].position[2] = .5f;
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
  surface.material = mat;
  remixapi_MeshInfo mesh{};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 2;
  mesh.surfaces_values = &surface;
  mesh.surfaces_count = 1;
  remixapi_MeshHandle geometry{};
  ok(api.CreateMesh(&mesh, &geometry));
  remixapi_InstanceIdentityEXT identity{};
  identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
  identity.instanceId = 1;
  identity.rasterVisible = 1;
  remixapi_InstanceInfoBlendEXT blend{};
  blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  blend.alphaBlendEnabled = 1;
  blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
  blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
  if (std::getenv("REMIX_TEST_OPAQUE_BLEND")) identity.pNext = &blend;
  if (std::getenv("REMIX_TEST_RAY_PRIMARY")) config("primaryVisibility", "ray");
  remixapi_InstanceInfo instance{};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.pNext = &identity;
  instance.mesh = geometry;
  instance.doubleSided = 1;
  for (int i = 0; i < 3; ++i)
    instance.transform.matrix[i][i] = 1;
  remixapi_InstanceIdentityEXT decalIdentity = identity;
  decalIdentity.instanceId = 2;
  remixapi_InstanceInfoBlendEXT decalBlend = blend;
  decalBlend.textureAlphaOperation = 3;
  decalBlend.textureAlphaArg1Source = 1;
  decalBlend.textureAlphaArg2Source = 3;
  decalBlend.tFactor = 0x80ffffff;
  decalIdentity.pNext = &decalBlend;
  remixapi_InstanceInfo decalInstance = instance;
  decalInstance.pNext = &decalIdentity;
  decalInstance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
  decalInstance.transform.matrix[2][3] = -.01f;
  const bool materialDecal = std::getenv("REMIX_TEST_MATERIAL_DECAL") != nullptr;
  std::array<remixapi_LightHandle, 3> lights{};
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = .1f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  for (unsigned i = 0; i < lights.size(); ++i) {
    light.hash = 10 + i;
    sphere.position = {float(i) * .4f - .4f, i == 1 ? .3f : -.2f, -.4f};
    light.radiance = {i == 0 ? 12.f : 2.f, i == 1 ? 12.f : 2.f, i == 2 ? 12.f : 2.f};
    ok(api.CreateLight(&light, &lights[i]));
  }
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, -1, 0};
  remixapi_LightInfo zeroSun{};
  zeroSun.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  zeroSun.pNext = &distant;
  zeroSun.hash = 20;
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&zeroSun, &sun));
  unsigned renderSequence = 0;
  bool lit = true;
  bool forceFresh = false;
  auto frame = remix::test::preparedFrame([&] {
    // Keep the measured lighting fixed while preventing stationary-frame reuse.
    if (forceFresh) {
      distant.direction.x = (++renderSequence & 1) ? .001f : 0;
      ok(api.UpdateLightDefinition(sun, &zeroSun));
    }
    ok(api.DrawLightInstance(sun));
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawInstance(&instance));
    if (materialDecal)
      ok(api.DrawInstance(&decalInstance));
    if (lit)
      for (auto handle : lights)
        ok(api.DrawLightInstance(handle));
    ok(api.Present(nullptr));
  });
  if (rrTest) {
    // SDK discovery is deferred until reconstruction is requested.
    config("upscaler", "dlss");
    frame();
    remixapi_ReconstructionDiagnosticsEXT d{}; d.structSize = sizeof(d);
    ok(remixapi_GetReconstructionDiagnosticsEXT(&d));
    if (!(d.capabilities & 4)) { ok(api.Shutdown()); return 77; }
    config("upscaler", "off");
  }
  int width = 0, height = 0;
  auto capture = [&](const std::string &name) {
    const auto path = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, path.c_str(), frame));
    frame();
    frame();
    int channels;
    auto *bytes = stbi_load(path.c_str(), &width, &height, &channels, 4);
    require(bytes, "Missing PBR denoising capture");
    Image image(size_t(width) * height);
    for (size_t i = 0; i < image.size(); ++i)
      for (int c = 0; c < 3; ++c) {
        double v = bytes[4 * i + c] / 255.;
        image[i][c] = std::pow(v, 2.2);
      }
    stbi_image_free(bytes);
    return image;
  };
  auto error = [&](const Image &a, const Image &b) {
    double sum = 0;
    unsigned count = 0;
    for (int y = height / 4; y < 3 * height / 4; ++y)
      for (int x = width / 4; x < 3 * width / 4; ++x)
        for (int c = 0; c < 3; ++c) {
          double d = a[y * width + x][c] - b[y * width + x][c];
          sum += d * d;
          ++count;
        }
    return sum / count;
  };
  auto energy = [&](const Image &a) {
    double sum = 0;
    unsigned count = 0;
    for (int y = height / 4; y < 3 * height / 4; ++y)
      for (int x = width / 4; x < 3 * width / 4; ++x)
        for (int c = 0; c < 3; ++c) {
          sum += a[y * width + x][c];
          ++count;
        }
    return sum / count;
  };
  std::array<std::filesystem::path, 3> maps = {"0x64", "0x65", "0x66"};
  for (unsigned map = 0; map < 3; ++map) {
    std::array<uint8_t, 8 * 8 * 4> pixels{};
    for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x) {
        bool alternate = (x + y) % 2;
        std::array<uint8_t, 4> color =
            map == 0   ? (alternate ? std::array<uint8_t, 4>{180, 80, 40, 255}
                                    : std::array<uint8_t, 4>{60, 120, 200, 255})
            : map == 1 ? std::array<uint8_t, 4>{uint8_t(alternate ? 128 : 204), 0, 0, 255}
                       : std::array<uint8_t, 4>{uint8_t(alternate ? 150 : 106), 128, 253, 255};
        std::copy(color.begin(), color.end(), pixels.begin() + 4 * (y * 8 + x));
      }
    remixapi_TextureInfo texture{};
    texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    texture.hash = 100 + map;
    texture.width = texture.height = 8;
    texture.depth = texture.mipLevels = 1;
    texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    texture.data = pixels.data();
    texture.dataSize = pixels.size();
    remixapi_TextureHandle handle;
    ok(api.CreateTexture(&texture, &handle));
  }
  for (unsigned kind = 0; kind < 3; ++kind) {
    std::string name = kind == 2 ? "textured" : kind ? "metal" : "dielectric";
    opaque.metallicConstant = kind == 1 ? 1.f : 0.f;
    opaque.roughnessConstant = kind ? .35f : .65f;
    if (kind == 2) {
      material.albedoTexture = maps[0].c_str();
      opaque.albedoConstant = {1, 1, 1};
      opaque.roughnessTexture = maps[1].c_str();
      material.normalTexture = maps[2].c_str();
    }
    ok(api.CreateMaterial(&material, &mat));
    config("separateLocalLighting", kind == 2 ? "1" : kind == 1 ? "2" : "0");
    ++epoch.historyResetEpoch;
    forceFresh = false;
    config("upscaler", "off");
    config("denoiser", "off");
    config("samples", "16");
    config("accumulate", "1");
    for (int i = 0; i < 192; ++i)
      frame();
    auto reference = capture(name + "-reference");
    forceFresh = true;
    config("accumulate", "0");
    config("samples", "1");
    for (int i = 0; i < 8; ++i)
      frame();
    auto raw = capture(name + "-raw");
    auto rawNext = capture(name + "-raw-next");
    config("denoiser", "reblur-sh");
    if (rrTest) config("upscaler", "dlss");
    for (int i = 0; i < 96; ++i)
      frame();
    auto filtered = capture(name + "-filtered");
    auto filteredNext = capture(name + "-filtered-next");
    remixapi_DenoisingStatisticsEXT stats{};
    stats.structSize = sizeof(stats);
    ok(remixapi_GetDenoisingStatisticsEXT(&stats));
    if (rrTest) {
      remixapi_ReconstructionDiagnosticsEXT d{};d.structSize=sizeof(d);
      ok(remixapi_GetReconstructionDiagnosticsEXT(&d));
      require(d.activeBackend==3 && d.evaluatedFrames>0, "RR PBR fallback");
      require(!stats.activeDenoiser && !stats.activeTaa, "RR PBR ran NRD/TAA");
    } else require(stats.activeDenoiser, "PBR denoiser silently fell back to raw");
    const double rawError = error(raw, reference), filteredError = error(filtered, reference);
    const double rawVariance = error(raw, rawNext), filteredVariance = error(filtered, filteredNext);
    std::printf("%s: error %.7f -> %.7f; temporal variance %.7f -> %.7f; energy %.5f -> %.5f\n", name.c_str(),
                rawError, filteredError, rawVariance, filteredVariance, energy(reference), energy(filtered));
    require(filteredError < rawError * .7, "PBR filtering did not reduce reference error");
    require(filteredVariance < rawVariance * .5, "PBR filtering did not reduce temporal noise");
    require(std::abs(energy(filtered) - energy(reference)) < .2 * energy(reference) + .003,
            "PBR denoising changed lighting energy");
  }
  // A camera cut into an unlit scene must not retain the previous highlights.
  lit = false;
  environment.radiance = {};
  ++epoch.historyResetEpoch;
  for (int i = 0; i < 4; ++i)
    frame();
  auto dark = capture("cut-to-dark");
  require(energy(dark) < .001, "Camera cut retained stale PBR lighting");
  ok(api.Shutdown());
  std::puts("PBR diffuse/specular denoising, energy and camera cuts passed");
}
