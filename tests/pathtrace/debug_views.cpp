#include "runtime/prepared.h"
#include <SDL3/SDL.h>
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
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void ok(remixapi_ErrorCode code) { require(!code, "Debug view API failure"); }
  using Color = std::array<float, 3>;
  using Image = std::vector<Color>;
  void near(Color a, Color b, float tolerance, const char *message) {
    for (unsigned c = 0; c < 3; ++c) require(std::abs(a[c] - b[c]) < tolerance, message);
  }
}
int main(int argc, char **argv) {
  const bool pbrOnly = argc == 3 && std::string(argv[2]) == "pbr";
  require(argc == 2 || pbrOnly, "Supply screenshot directory and optional pbr mode");
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
  startup.windowWidth = startup.windowHeight = 96;
#endif
  ok(api.Startup(&startup));
  // These fixtures measure the unexposed lighting/display contract.
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "off"));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2)) { ok(api.Shutdown()); return 77; }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  for (auto key : {"sharc", "sigma", "localGuiding"}) config(key, "0");
  config("bounces", "1");
  config("samples", "1");
  for (const char *bad : {"12", "-1", "1.5", "garbage"})
    require(api.SetConfigVariable("rtx.native.pathtrace.debugView", bad) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "Invalid diagnostic accepted");
  uint64_t next = 1;
  std::vector<remixapi_MaterialHandle> materials;
  std::vector<remixapi_MeshHandle> meshes;
  auto quad = [&](float z, remixapi_Float3D color, float alpha, float emission = 0.f) {
    remixapi_MaterialInfoPbrEXT pbr{};
    pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT; pbr.enabled = pbrOnly;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &pbr; opaque.roughnessConstant = .65f;
    opaque.albedoConstant = color; opaque.opacityConstant = alpha; opaque.alphaTestType = 7;
    remixapi_MaterialInfo mat{};
    mat.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO; mat.hash = next++; mat.pNext = &opaque;
    mat.emissiveIntensity = emission; mat.emissiveColorConstant = color;
    remixapi_MaterialHandle material{}; ok(api.CreateMaterial(&mat, &material)); materials.push_back(material);
    remixapi_HardcodedVertex v[4]{};
    for (int i = 0; i < 4; ++i) {
      v[i].position[0] = emission ? (i == 0 || i == 3 ? -.8f : -.4f) : (i == 0 || i == 3 ? -2.f : 2.f);
      v[i].position[1] = emission ? (i < 2 ? -.8f : -.4f) : (i < 2 ? -2.f : 2.f);
      v[i].position[2] = z;
      v[i].color = 0xffffffff;
    }
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = v; surface.vertices_count = 4;
    surface.indices_values = indices; surface.indices_count = 6; surface.material = material;
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO; mesh.hash = next++;
    mesh.surfaces_values = &surface; mesh.surfaces_count = 1;
    remixapi_MeshHandle handle{}; ok(api.CreateMesh(&mesh, &handle)); meshes.push_back(handle); return handle;
  };
  const auto receiver = quad(2, {.3f, .6f, .9f}, 1);
  const auto glass = quad(1, {.1f, 1, .2f}, .5f);
  const auto emitter = pbrOnly ? quad(1, {0, 0, 1}, 1, 4) : nullptr;
  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1; environment.radiance = {.2f, .4f, .6f};
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT; epoch.worldEpoch = 1; epoch.pNext = &environment;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO; camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i) camera.view[i][i] = 1;
  camera.projection[0][0] = camera.projection[1][1] = 2;
  camera.projection[2][2] = 100.f / 99.95f; camera.projection[2][3] = 1;
  camera.projection[3][2] = -5.f / 99.95f;
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT; distant.direction = {0, 0, 1};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; light.pNext = &distant; light.hash = next++;
  remixapi_LightHandle sun{}; ok(api.CreateLight(&light, &sun)); // zero sunlight
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT; sphere.position = {0, 0, 1}; sphere.radius = .05f;
  light.pNext = &sphere; light.hash = next++; light.radiance = {40, 0, 0};
  remixapi_LightHandle torch{}; ok(api.CreateLight(&light, &torch));
  bool showGlass = false, showTorch = true, showEmitter = false;
  auto draw = [&](remixapi_MeshHandle mesh) {
    remixapi_InstanceIdentityEXT id{};
    id.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    id.instanceId = reinterpret_cast<uintptr_t>(mesh); id.rasterVisible = 1;
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT; blend.pNext = &id;
    blend.alphaBlendEnabled = mesh == glass;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO; instance.pNext = &blend;
    instance.mesh = mesh; instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i) instance.transform.matrix[i][i] = 1;
    ok(api.DrawInstance(&instance));
  };
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    if (showTorch)
      ok(api.DrawLightInstance(torch));
    draw(receiver);
    if (showGlass)
      draw(glass);
    if (showEmitter)
      draw(emitter);
    ok(api.Present(nullptr));
  });
  auto stats = [&] {
    remixapi_DenoisingStatisticsEXT s{}; s.structSize = sizeof(s);
    ok(remixapi_GetDenoisingStatisticsEXT(&s)); return s;
  };
  int width = 0, height = 0;
  auto capture = [&](const std::string &name, unsigned view) {
    config("debugView", std::to_string(view).c_str());
    const auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int channels;
    auto *data = stbi_load(file.c_str(), &width, &height, &channels, 4);
    require(data, "Missing debug capture");
    Image image(width * height);
    for (size_t i = 0; i < image.size(); ++i) for (unsigned c = 0; c < 3; ++c) {
      float v = data[i * 4 + c] / 255.f;
      image[i][c] = std::pow(v, 2.2f);
    }
    stbi_image_free(data); return image;
  };
  auto sample = [&](const Image &image) { return image[(height / 2 - 10) * width + width / 2 + 10]; };
  auto localMean = [&](const Image &image) {
    // A mixed-light pixel can select only one family. Average a receiver patch
    // clear of the visible emitter so both sampled families have coverage.
    Color result{};
    for (int y = height / 2 - 16; y < height / 2 - 4; ++y)
      for (int x = width / 2 + 4; x < width / 2 + 16; ++x)
        for (unsigned c = 0; c < 3; ++c) result[c] += image[y * width + x][c] / 144.f;
    return result;
  };
  Image referenceIds;
  if (pbrOnly) {
    config("bounces", "0"); config("taa", "0");
    config("denoiser", "reblur-sh"); config("localSamples", "16");
    config("localFireflyFilter", "1");
    for (const char *primary : {"raster", "ray"}) for (unsigned family = 0; family < 3; ++family) {
      config("primaryVisibility", primary);
      showTorch = family != 1; showEmitter = family != 0;
      for (const char *split : {"0", "1", "2", "3"}) for (const char *filtered : {"0", "1"}) {
        config("separateLocalLighting", split); config("localDenoiser", filtered);
        config("debugView", "0");
        for (unsigned i = 0; i < 8; ++i) frame();
        const auto initial = stats();
        const auto prefix = std::string(primary) + "-pbr-" + std::to_string(family) + "-split" + split + "-filter" + filtered;
        const auto local = localMean(capture(prefix + "-local", 6));
        if (showTorch) require(local[0] > .005f, "PBR torch lighting absent from raw local diagnostic");
        if (showEmitter) require(local[2] > .005f, "PBR emissive lighting absent from raw local diagnostic");
        near(sample(capture(prefix + "-indirect", 7)), {0, 0, 0}, .001f, "PBR zero-bounce indirect debug not black");
        require(stats().historyResets == initial.historyResets, "PBR diagnostics reset lighting history");
        require(stats().activeDenoiser == initial.activeDenoiser, "PBR diagnostics changed active denoiser");
        std::printf("PBR local diagnostics verified: %s\n", prefix.c_str());
      }
    }
  } else for (const char *primary : {"raster", "ray"}) for (unsigned mode = 0; mode < 4; ++mode) {
    config("primaryVisibility", primary);
    config("denoiser", mode >= 2 ? "reblur-sh" : "off");
    config("taa", mode ? "1" : "0");
    config("upscaler", mode == 3 ? "fsr31" : "off");
    config("fsrQuality", "performance"); // exercise diagnostic presentation at half resolution
    config("debugView", "0");
    for (unsigned i = 0; i < 40; ++i) frame();
    std::string prefix = std::string(primary) + "-" + std::to_string(mode) + "-";
    auto before = sample(capture(prefix + "final-before", 0));
    const auto initial = stats();
    std::array<Image, 12> images;
    for (unsigned view = 1; view <= 11; ++view) images[view] = capture(prefix + std::to_string(view), view);
    near(sample(images[1]), {.3f, .6f, .9f}, .012f, "Albedo debug altered by lighting/resolve");
    near(sample(images[2]), {.5f, .5f, 0}, .01f, "Normal debug incorrect");
    auto ids = capture(prefix + "ids-repeat", 3);
    for (int y = 12; y < height - 12; ++y) for (int x = 12; x < width - 12; ++x)
      if (std::abs(x + y - width + 1) > 6)
        near(ids[y * width + x], images[3][y * width + x], .001f, "Stationary triangle identity changed");
    if (!mode) {
      if (referenceIds.empty()) referenceIds = ids;
      else for (int y = 12; y < height - 12; ++y) for (int x = 12; x < width - 12; ++x)
        if (std::abs(x + y - width + 1) > 6)
          near(ids[y * width + x], referenceIds[y * width + x], .001f, "Raster/ray triangle identities disagree");
    }
    near(sample(images[7]), {.2f, .4f, .6f}, .012f, "Raw indirect is filtered or albedo-modulated");
    auto local = sample(images[6]);
    require(local[0] > .03f && local[1] < .002f && local[2] < .002f, "Raw local signal mixed with other lighting");
    if (mode) {
      near(sample(images[8]), {.5f, .5f, .5f}, .012f, "Stationary motion not zero");
      near(sample(images[9]), {0, 1, 0}, .001f, "Stable correspondence invalid");
      require(stats().historyResets == initial.historyResets, "Debug selection reset temporal history");
      require(stats().activeDenoiser == initial.activeDenoiser && stats().activeTaa == initial.activeTaa,
              "Debug selection changed active filters");
      if (mode != 3) require(sample(images[11])[0] < .8f, "Debug selection discarded TAA history");
      config("debugView", "8");
      camera.view[3][0] = .1f;
      auto moving = sample(capture(prefix + "moving", 8));
      require(std::abs(moving[0] - .5f) > .025f && std::abs(moving[1] - .5f) < .01f,
              "Camera translation absent from motion diagnostic");
      camera.view[3][0] = 0;
      for (unsigned i = 0; i < 8; ++i) frame();
    }
    if (!mode) {
      auto unavailable = sample(images[8]);
      require(unavailable[0] > .02f && std::abs(unavailable[0] - unavailable[2]) < .01f,
              "Unavailable temporal debug not indicated");
    }
    if (mode >= 2) require(sample(images[10])[0] > .8f, "Stable indirect confidence unexpectedly low");
    auto after = sample(capture(prefix + "final-after", 0));
    near(before, after, .035f, "Diagnostic contaminated final lighting history");
    showGlass = true;
    near(sample(capture(prefix + "glass-albedo", 1)), {.1f, 1, .2f}, .012f,
         "Geometry debug does not show first transparent surface");
    showGlass = false;
    config("bounces", "0");
    near(sample(capture(prefix + "zero-bounce", 7)), {0, 0, 0}, .001f, "Zero bounce indirect debug not black");
    config("bounces", "1");
    if (mode >= 2) {
      config("separateLocalLighting", "1");
      auto split = sample(capture(prefix + "split-local", 6));
      near(split, local, .02f, "Deferred split local lighting absent from diagnostic");
      config("separateLocalLighting", "0");
    }
    std::printf("Debug views verified: %s mode %u\n", primary, mode);
  }
  for (auto mesh : meshes) ok(api.DestroyMesh(mesh));
  for (auto material : materials) ok(api.DestroyMaterial(material));
  ok(api.DestroyLight(torch)); ok(api.DestroyLight(sun)); ok(api.Shutdown());
}
