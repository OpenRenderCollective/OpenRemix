#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <string>
#include <vector>

namespace {
  void captureEnvironment(const std::string &path) {
    // The renderer reads the executable's C runtime environment. SDL may use a
    // different CRT on Windows, so changing SDL's copy does not reach getenv.
#ifdef _WIN32
    _putenv_s("REMIX_PATH_LINEAR_CAPTURE", path.c_str());
#else
    if (path.empty()) unsetenv("REMIX_PATH_LINEAR_CAPTURE");
    else setenv("REMIX_PATH_LINEAR_CAPTURE", path.c_str(), 1);
#endif
  }
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void ok(remixapi_ErrorCode code, std::source_location where = std::source_location::current()) {
    if (code) {
      std::fprintf(stderr, "Measured-sheet API failure %x at line %u\n", unsigned(code), where.line());
      std::exit(1);
    }
  }
  std::array<double, 3> mean(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) std::fprintf(stderr, "Missing measured-sheet capture: %s\n", path.string().c_str());
    std::string magic;
    unsigned width, height;
    float scale;
    input >> magic >> width >> height >> scale;
    input.get();
    require(input.good() && magic == "PF" && scale == -1 && width == 96 && height == 64,
            "Measured-sheet linear capture header");
    std::vector<float> pixels(size_t(width) * height * 3);
    input.read(reinterpret_cast<char *>(pixels.data()), pixels.size() * sizeof(float));
    require(input.good(), "Measured-sheet linear capture pixels");
    std::array<double, 3> result{};
    for (unsigned y = height / 4; y < 3 * height / 4; ++y)
      for (unsigned x = width / 4; x < 3 * width / 4; ++x)
        for (unsigned c = 0; c < 3; ++c) {
          const float value = pixels[(y * width + x) * 3 + c];
          require(std::isfinite(value) && value >= 0, "Measured-sheet radiance is finite and nonnegative");
          result[c] += value / double((width / 2) * (height / 2));
        }
    return result;
  }
}
int main(int argc, char **argv) {
  require(argc == 2, "Supply measured-sheet capture directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  ok(api.Startup(&startup));
  ok(api.SetConfigVariable("rtx.native.asyncPresent", "0"));
  ok(api.SetConfigVariable("rtx.native.lowLatency", "0"));
  int windowCount = 0;
  SDL_Window **windows = SDL_GetWindows(&windowCount);
  require(windows && windowCount > 0, "Measured-sheet window");
  SDL_SetWindowSize(windows[0], 96, 64);
  SDL_free(windows);
  remixapi_BackendCapabilitiesEXT capabilities{};
  capabilities.structSize = sizeof(capabilities);
  ok(remixapi_GetBackendCapabilitiesEXT(&capabilities));
  if (!(capabilities.supportedBackends & 2)) { ok(api.Shutdown()); return 77; }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  auto config = [&](const char *name, const char *value) {
    const auto error = api.SetConfigVariable((std::string("rtx.native.pathtrace.") + name).c_str(), value);
    if (error) std::fprintf(stderr, "Configuration %s=%s\n", name, value);
    ok(error);
  };
  for (const char *name : {"autoExposure", "bloom", "taa", "sigma", "sharc", "prism", "gamma22",
                           "skipBlendedSecondaryRays", "waterIndirectReuse", "translucencyReuse"}) config(name, "0");
  config("denoiser", "off");
  config("upscaler", "off");
  config("samples", "16");
  config("bounces", "2");
  config("sheetLocalLights", "1");
  uint64_t id = 100;
  auto mesh = [&](remixapi_MaterialHandle material) {
    remixapi_HardcodedVertex vertices[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      vertices[i].position[0] = i == 0 || i == 3 ? -100 : 100;
      vertices[i].position[1] = i < 2 ? -100 : 100;
      vertices[i].color = 0xffffffff;
    }
    const uint32_t indices[] = {0, 2, 1, 0, 3, 2};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices; surface.vertices_count = 4;
    surface.indices_values = indices; surface.indices_count = 6; surface.material = material;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = id++; info.surfaces_values = &surface; info.surfaces_count = 1;
    remixapi_MeshHandle result;
    ok(api.CreateMesh(&info, &result));
    return result;
  };
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.opacityConstant = 1; opaque.alphaTestType = 7;
  remixapi_MaterialInfo backgroundInfo{};
  backgroundInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  backgroundInfo.pNext = &opaque; backgroundInfo.hash = id++;
  backgroundInfo.emissiveColorConstant = {1, 1, 1}; backgroundInfo.emissiveIntensity = .2f;
  remixapi_MaterialHandle material;
  ok(api.CreateMaterial(&backgroundInfo, &material));
  auto background = mesh(material);
  remixapi_LightInfoDomeEXT dome{};
  dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
  for (unsigned c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; light.hash = id++;
  light.pNext = &dome; light.radiance = {.4f, .4f, .4f};
  remixapi_LightHandle environment;
  ok(api.CreateLight(&light, &environment));
  remixapi_MeshHandle pane = nullptr;
  remixapi_MeshHandle indirectWall = nullptr;
  remixapi_LightHandle indirectSun = nullptr;
  float backgroundDistance = .7f;
  unsigned layers = 1;
  bool reversed = false;
  const bool indirectTest = std::getenv("REMIX_TEST_SHEET_INDIRECT") != nullptr;
  remixapi_CameraEnvironmentEXT uniformEnvironment{};
  uniformEnvironment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  uniformEnvironment.enabled = 1;
  uniformEnvironment.radiance = {.4f, .4f, .4f};
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    remixapi_CameraInfo camera{};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    if (indirectTest) camera.pNext = &uniformEnvironment;
    for (unsigned c = 0; c < 4; ++c) camera.view[c][c] = camera.projection[c][c] = 1;
    ok(api.SetupCamera(&camera));
    if (!indirectTest) ok(api.DrawLightInstance(environment));
    if (indirectSun) ok(api.DrawLightInstance(indirectSun));
    auto draw = [&](remixapi_MeshHandle handle, float z, bool transparent, uint64_t identity) {
      remixapi_InstanceIdentityEXT stable{};
      stable.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      stable.instanceId = identity; stable.rasterVisible = 1;
      remixapi_InstanceInfoBlendEXT blend{};
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      blend.pNext = &stable; blend.alphaBlendEnabled = transparent;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO; instance.pNext = &blend;
      instance.mesh = handle; instance.doubleSided = 1;
      for (unsigned c = 0; c < 3; ++c) instance.transform.matrix[c][c] = 1;
      if (transparent && reversed) instance.transform.matrix[1][1] = -1;
      if (identity == 5) instance.transform.matrix[1][1] = -1;
      instance.transform.matrix[2][3] = z;
      ok(api.DrawInstance(&instance));
    };
    draw(background, backgroundDistance, false, 1);
    if (indirectWall) draw(indirectWall, -.5f, false, 5);
    if (pane) for (unsigned layer = 0; layer < layers; ++layer) draw(pane, .2f + .3f * layer, true, 2 + layer);
    ok(api.Present(nullptr));
  });
  auto capture = [&](const std::string &name) {
    for (unsigned i = 0; i < 8; ++i) frame();
    const auto path = directory / (name + ".pfm");
    captureEnvironment(path.string());
    frame();
    captureEnvironment({});
    for (unsigned i = 0; i < 3; ++i) frame();
    return mean(path);
  };
  if (indirectTest) {
    // A broad lit wall behind the camera has direct outgoing radiance .4.
    // A grazing sun lights that wall while passing outside the two other
    // planes; the camera's receiver faces away from it. This gives the same
    // angular integral as a uniform sky, plus actual secondary cache queries.
    // Low wall reflectance bounds SHARC's extra interreflection tail below 1%:
    // its round-trip gain is at most .8 * .01, even without sheet absorption.
    constexpr float wallReflectance = .01f;
    opaque.albedoConstant = {wallReflectance, wallReflectance, wallReflectance};
    backgroundInfo.hash = id++;
    backgroundInfo.emissiveIntensity = 0;
    ok(api.CreateMaterial(&backgroundInfo, &material));
    indirectWall = mesh(material);
    remixapi_LightInfoDistantEXT grazingSun{};
    grazingSun.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    grazingSun.direction = {-1, 0, -.001f};
    light.hash = id++; light.pNext = &grazingSun;
    const float sunRadiance = float(.4 * 3.141592653589793 * std::sqrt(1.000001) /
                                   (.001 * wallReflectance));
    light.radiance = {sunRadiance, sunRadiance, sunRadiance};
    ok(api.CreateLight(&light, &indirectSun));
    opaque.albedoConstant = {.8f, .8f, .8f};
    remixapi_MaterialInfoPbrEXT legacy{};
    legacy.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    legacy.pNext = &opaque;
    backgroundInfo.pNext = &legacy;
    backgroundInfo.hash = id++;
    backgroundInfo.emissiveIntensity = 0;
    ok(api.CreateMaterial(&backgroundInfo, &material));
    background = mesh(material);
    remixapi_MaterialInfoTranslucentEXT sheet{};
    sheet.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    sheet.refractiveIndex = 1;
    sheet.transmittanceColor = {.0625f, .25f, 1};
    sheet.transmittanceMeasurementDistance = .2f;
    sheet.thinWallThickness_hasvalue = 1;
    sheet.thinWallThickness_value = .05f;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.hash = id++; info.pNext = &sheet;
    ok(api.CreateMaterial(&info, &material));
    const auto tintedPane = mesh(material);
    const double tint[] = {.0625, .25, 1};
    std::array<double, 3> expected{};
    constexpr unsigned quadratureSteps = 16384;
    for (unsigned c = 0; c < 3; ++c) {
      const double depth = -std::log(tint[c]) * .25;
      double hemisphericalTransmission = 0;
      for (unsigned i = 0; i < quadratureSteps; ++i) {
        const double cosine = (i + .5) / quadratureSteps;
        hemisphericalTransmission += 2 * cosine * std::exp(-depth / cosine) / quadratureSteps;
      }
      expected[c] = .32 * std::exp(-depth) * hemisphericalTransmission;
    }
    for (const char *primary : {"ray", "raster"}) for (bool cached : {false, true}) {
      config("primaryVisibility", primary);
      config("sharc", cached ? "1" : "0");
      for (bool tinted : {false, true}) {
        pane = tinted ? tintedPane : nullptr;
        for (unsigned i = 0; i < 128; ++i) frame();
        const auto name = std::string(primary) + (cached ? "-cached" : "-reference") +
            (tinted ? "-tinted-indirect" : "-open-indirect");
        const auto image = capture(name);
        for (unsigned c = 0; c < 3; ++c) {
          const double target = tinted ? expected[c] : .32;
          std::fprintf(stderr, "%s channel %u: %.6f expected %.6f\n", name.c_str(), c, image[c], target);
          require(std::abs(image[c] - target) < .02, "Measured-sheet indirect transport matches hemisphere integral");
        }
        if (cached) {
          remixapi_SharcStatisticsEXT statistics{};
          statistics.structSize = sizeof(statistics);
          ok(remixapi_GetSharcStatisticsEXT(&statistics));
          require(statistics.active && statistics.occupiedEntries && statistics.queryHits,
                  "Measured-sheet cache comparison must exercise populated SHARC queries");
        }
      }
    }
    ok(api.Shutdown());
    return 0;
  }
  for (const char *primary : {"ray", "raster"}) {
    config("primaryVisibility", primary);
    pane = nullptr;
    const auto baseline = capture(std::string(primary) + "-background");
    for (double value : baseline) require(std::abs(value - .2) < .01, "Emissive background radiometric reference");
    for (float index : {1.f, 1.5f}) for (bool tinted : {false, true}) {
      remixapi_MaterialInfoTranslucentEXT sheet{};
      sheet.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
      sheet.refractiveIndex = index;
      sheet.transmittanceColor = tinted ? remixapi_Float3D{.0625f, .25f, 1} : remixapi_Float3D{1, 1, 1};
      sheet.transmittanceMeasurementDistance = .2f;
      sheet.thinWallThickness_hasvalue = 1; sheet.thinWallThickness_value = .05f;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO; info.hash = id++; info.pNext = &sheet;
      ok(api.CreateMaterial(&info, &material));
      pane = mesh(material);
      for (float distance : {.7f, .9f}) for (bool back : {false, true}) {
        backgroundDistance = distance; reversed = back;
        const auto name = std::string(primary) + "-" + std::to_string(index) + (tinted ? "-tint" : "-clear") +
            "-" + std::to_string(distance) + (back ? "-back" : "-front");
        const auto image = capture(name);
        const float color[] = {sheet.transmittanceColor.x, sheet.transmittanceColor.y, sheet.transmittanceColor.z};
        const float f = std::pow((index - 1) / (index + 1), 2);
        for (unsigned c = 0; c < 3; ++c) {
          const float a = std::pow(color[c], .25f), denominator = 1 - f * f * a * a;
          const float passed = (1 - f) * (1 - f) * a / denominator;
          const float reflected = f + (1 - f) * (1 - f) * f * a * a / denominator;
          const double expected = .2 * passed + .4 * reflected;
          std::fprintf(stderr, "%s channel %u: %.6f expected %.6f\n", name.c_str(), c, image[c], expected);
          require(std::abs(image[c] - expected) < .012, "Primary measured-sheet radiance matches both interfaces without diffuse glow");
        }
        if (index == 1) {
          layers = 2;
          const auto layered = capture(name + "-layers");
          layers = 1;
          for (unsigned c = 0; c < 3; ++c)
            require(std::abs(layered[c] - .2 * std::pow(color[c], .5f)) < .012,
                    "Stacked index-matched sheets multiply finite transmission");
        }
      }
    }
  }
  ok(api.Shutdown());
}
