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
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void ok(remixapi_ErrorCode result) { require(!result, "Environment test API failure"); }
}
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
    std::printf("Sky scenes unavailable: %s\n", caps.unavailableReason);
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  // These radiometric checks decode the gamma 2.2 output.
  ok(api.SetConfigVariable("rtx.native.pathtrace.samples", "16"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "1"));
  require(api.SetConfigVariable("rtx.native.pathtrace.sky", "2") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid sky toggle accepted");
  ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "4"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.sharcCapacityLog2", "18"));
  // The eligibility checks below describe the cache without entries on sharp
  // or moving surfaces.
  ok(api.SetConfigVariable("rtx.native.pathtrace.sharcGlossySurfaces", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.sharcDynamicSurfaces", "0"));
  ok(api.SetConfigVariable("rtx.native.vsync", "False"));
  for (auto key : {"sharc", "sharcDebug", "sharcGlossySurfaces", "sharcDynamicSurfaces"}) {
    std::string setting = std::string("rtx.native.pathtrace.") + key;
    require(api.SetConfigVariable(setting.c_str(), "2") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "Invalid SHARC toggle accepted");
  }
  for (auto key : {"sharcCapacityLog2", "sharcDownscale", "sharcSceneScale", "sharcAccumulationFrames", "sharcStaleFrames"}) {
    std::string setting = std::string("rtx.native.pathtrace.") + key;
    require(api.SetConfigVariable(setting.c_str(), "0") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS &&
            api.SetConfigVariable(setting.c_str(), "999999999") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "Invalid SHARC setting accepted");
  }
  uint64_t next = 100;
  const bool pbrScene = std::getenv("REMIX_TEST_SHARC_PBR");
  auto material = [&](remixapi_Float3D color, float alpha, bool cutout = false, bool pbr = false,
                      float roughness = 1, float metallic = 0, const std::filesystem::path &normal = {},
                      const std::filesystem::path &roughnessMap = {}) {
    pbr |= pbrScene;
    remixapi_MaterialInfoPbrEXT mode{};
    mode.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT; mode.enabled = pbr;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &mode;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.roughnessConstant = pbr ? roughness : 0.f;
    opaque.metallicConstant = metallic;
    opaque.roughnessTexture = roughnessMap.c_str();
    opaque.alphaTestType = cutout ? 4 : 7; // GREATER .5
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.normalTexture = normal.c_str();
    info.hash = next++;
    remixapi_MaterialHandle handle{};
    ok(api.CreateMaterial(&info, &handle));
    return handle;
  };
  const auto white = material({1, 1, 1}, 1), black = material({.25f, .25f, .25f}, 1);
  const auto cutout = material({1, 1, 1}, 0, true), tint = material({.1f, 1, .2f}, .5f);
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1, remixapi_MaterialHandle mat) {
    remixapi_HardcodedVertex vertices[4]{};
    for (int i = 0; i < 4; ++i) {
      vertices[i].position[axis] = plane;
      vertices[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      vertices[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      vertices[i].texcoord[0] = i == 0 || i == 3 ? 0 : 1;
      vertices[i].texcoord[1] = i < 2 ? 0 : 1;
      vertices[i].color = 0xffffffff;
    }
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices; surface.vertices_count = 4;
    surface.indices_values = indices; surface.indices_count = 6; surface.material = mat;
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = next++; mesh.surfaces_values = &surface; mesh.surfaces_count = 1;
    remixapi_MeshHandle handle{};
    ok(api.CreateMesh(&mesh, &handle));
    return handle;
  };
  const auto receiver = quad(2, .5f, -2, 2, -2, 2, white);
  const auto sharpReceiver = quad(2, .5f, -2, 2, -2, 2, material({1, 1, 1}, 1, false, true, 0));
  const auto offscreenReceiver = quad(2, .5f, 10, 12, -2, 2, white);
  bool sharpOnly = false, dynamicOnly = false;
  remixapi_MeshHandle receiverOverride{};
  std::vector<remixapi_MeshHandle> sides;
  for (float sign : {-1.f, 1.f}) {
    sides.push_back(quad(0, sign, -1, 1, -1, .5f, black));
    sides.push_back(quad(1, sign, -1, .5f, -1, 1, black));
  }
  const auto back = quad(2, -1, -1, 1, -1, 1, black);
  const auto mover = quad(2, -1, -1, 1, -1, 1, black);
  float moverOffset = 3;
  const auto holes = quad(2, -1, -1, 1, -1, 1, cutout);
  const auto glass = quad(2, -1, -1, 1, -1, 1, tint);
  const std::vector<remixapi_MeshHandle> opening = {
    quad(2, -1, -1, -.5f, -1, 1, black), quad(2, -1, .5f, 1, -1, 1, black),
    quad(2, -1, -.5f, .5f, -1, -.5f, black), quad(2, -1, -.5f, .5f, .5f, 1, black)};
  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1;
  environment.radiance = {.25f, .5f, .75f};
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1; epoch.pNext = &environment;
  float translation = 0;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO; camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i) camera.view[i][i] = camera.projection[i][i] = 1;
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, 0, 1};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; light.pNext = &distant; light.hash = next++;
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&light, &sun));
  std::vector<remixapi_MeshHandle> enclosure;
  auto draw = [&](remixapi_MeshHandle mesh) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = reinterpret_cast<uintptr_t>(mesh); identity.rasterVisible = 1;
    identity.classification = dynamicOnly || mesh == mover ? 1 : 0;
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT; blend.pNext = &identity;
    blend.alphaBlendEnabled = mesh == glass;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO; instance.pNext = &blend;
    instance.mesh = mesh; instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i) instance.transform.matrix[i][i] = 1;
    instance.transform.matrix[0][3] = translation + (mesh == mover ? moverOffset : 0);
    ok(api.DrawInstance(&instance));
  };
  auto frame = remix::test::preparedFrame([&] {
    camera.view[3][0] = -translation;
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    draw(receiverOverride ? receiverOverride : sharpOnly ? sharpReceiver : receiver);
    for (auto mesh : enclosure)
      draw(mesh);
    ok(api.Present(nullptr));
  });
  auto temporal = [&] {
    remixapi_DenoisingStatisticsEXT stats{}; stats.structSize = sizeof(stats);
    ok(remixapi_GetDenoisingStatisticsEXT(&stats)); return stats;
  };
  auto tracing = [&] {
    remixapi_TracingStatisticsEXT stats{}; stats.structSize = sizeof(stats);
    ok(remixapi_GetTracingStatisticsEXT(&stats)); return stats;
  };
  auto readback = [&](const std::string &name) {
    const auto file = (directory / (name + ".png")).string();
    const auto raw = (directory / (name + ".pfm")).string();
    if (pbrScene) SDL_setenv_unsafe("REMIX_PATH_LINEAR_CAPTURE", raw.c_str(), 1);
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    if (pbrScene) SDL_unsetenv_unsafe("REMIX_PATH_LINEAR_CAPTURE");
    frame();
    int width, height, channels;
    auto pixels = stbi_load(file.c_str(), &width, &height, &channels, 4);
    require(pixels, "Missing sky screenshot");
    std::array<float, 3> average{};
    for (int y = height / 2 - 4; y < height / 2 + 4; ++y)
      for (int x = width / 2 - 4; x < width / 2 + 4; ++x)
        for (int c = 0; c < 3; ++c) {
          float v = pixels[(y * width + x) * 4 + c] / 255.f;
          average[c] += (std::pow(v, 2.2f)) / 64;
        }
    stbi_image_free(pixels);
    if (pbrScene) {
      frame(); frame(); // Retire all three readback slots.
      std::ifstream input(raw, std::ios::binary);
      std::string magic; float scale = 0;
      input >> magic >> width >> height >> scale;
      require(magic == "PF" && scale == -1 && width > 0 && height > 0, "Missing linear PBR cache capture");
      input.get();
      std::vector<float> radiance(size_t(width) * height * 3);
      input.read(reinterpret_cast<char *>(radiance.data()), radiance.size() * sizeof(float));
      require(bool(input), "Truncated linear PBR cache capture");
      for (auto v : radiance) require(std::isfinite(v) && v >= 0, "Invalid PBR cache radiance");
      average = {};
      for (int y = height / 2 - 4; y < height / 2 + 4; ++y)
        for (int x = width / 2 - 4; x < width / 2 + 4; ++x)
          for (int c = 0; c < 3; ++c) average[c] += radiance[(y * width + x) * 3 + c] / 64;
    }
    std::printf("%s linear RGB: %.7f %.7f %.7f\n", name.c_str(), average[0], average[1], average[2]);
    return average;
  };
  auto capture = [&](const std::string &name, int frames) {
    // Independent scenes use explicit cuts; live-change checks below do not.
    ++epoch.historyResetEpoch;
    for (int i = 0; i < frames; ++i) frame();
    return readback(name);
  };
  auto cache = [&] {
    remixapi_SharcStatisticsEXT value{}; value.structSize = sizeof(value);
    ok(remixapi_GetSharcStatisticsEXT(&value)); return value;
  };
  remixapi_SharcStatisticsEXT invalid{};
  require(remixapi_GetSharcStatisticsEXT(nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS &&
          remixapi_GetSharcStatisticsEXT(&invalid) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Malformed SHARC statistics accepted");
  auto configure = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  if (std::getenv("REMIX_TEST_FAULT")) configure("sigma", "0");
  configure("denoiser", "off"); configure("taa", "0");
  frame(); frame();
  if (const char *fault = std::getenv("REMIX_TEST_FAULT")) {
    if (std::string(fault).starts_with("sharc-")) {
      require(!cache().active && cache().unavailableReason[0] && tracing().sampleSequence,
              "SHARC failure did not retain uncached tracing");
      auto value = readback("fallback");
      require(value[1] > .4f, "SHARC failure lost reference lighting");
      ok(api.Shutdown()); std::puts("SHARC unavailable/allocation fallback passed"); return 0;
    }
  }
  if (!cache().supported) {
    require(!cache().active && cache().unavailableReason[0] && tracing().sampleSequence,
            "Unavailable SHARC did not retain tracing");
    std::printf("SHARC unavailable: %s\n", cache().unavailableReason);
    ok(api.Shutdown()); return 77;
  }
  require(cache().active && cache().memoryBytes >= (uint64_t(1) << 18) * 64, "Directional SHARC did not initialize");
  for (const char *primary : {"raster", "ray"}) {
    configure("primaryVisibility", primary);
    for (int scene = 0; scene < 4; ++scene) {
      enclosure = sides;
      if (scene == 0) enclosure.push_back(back);
      if (scene == 1) enclosure.insert(enclosure.end(), opening.begin(), opening.end());
      if (scene == 2) enclosure.push_back(holes);
      if (scene == 3) enclosure.push_back(glass);
      const std::string label = std::string(primary) + "-" + std::to_string(scene);
      configure("accumulate", "1");
      auto reference = capture(label + "-reference", 48);
      require(!cache().active, "Progressive reference used SHARC");
      configure("accumulate", "0"); configure("sharc", "1");
      auto actual = capture(label + "-cache", 160);
      for (int c = 0; c < 3; ++c)
        require(std::abs(actual[c] - reference[c]) < .055f, "SHARC differs excessively from diffuse reference");
      require(cache().active && cache().queryHits && cache().occupiedEntries,
              "Cache comparison did not exercise successful indirect queries");
      if (scene == 0)
        require(actual[0] < .005f && actual[1] < .005f && actual[2] < .005f, "SHARC leaked into enclosed room");
      if (scene == 3)
        require(actual[0] < actual[1] * .4f && actual[2] < actual[1] * .65f, "Cache lost tinted occlusion");
      auto stats = cache();
      std::printf("%s cache entries=%u sampled hits=%u/%u update=%.3f resolve=%.3f ms\n", label.c_str(),
          stats.occupiedEntries, stats.queryHits, stats.queryAttempts, stats.gpuUpdateMilliseconds, stats.gpuResolveMilliseconds);
    }
    // Ordinary camera movement retains the cache; an origin rebase uses the explicit world epoch.
    auto resets = cache().resets;
    camera.view[3][1] = .05f; frame(); frame();
    require(cache().resets == resets, "Ordinary camera move cleared SHARC");
    camera.view[3][1] = 0;
    translation = -16; ++epoch.worldEpoch; frame(); frame();
    require(cache().resets > resets, "World/origin rebase retained stale SHARC keys");
    auto moved = capture(std::string(primary) + "-negative-coordinates", 96);
    require(moved[1] > .02f, "SHARC failed at negative coordinates");
    resets = cache().resets;
    configure("sharcReset", "1"); frame(); frame();
    require(cache().resets == resets + 1, "Explicit cache reset failed");
    translation = 0; ++epoch.worldEpoch;
    // Live lighting changes retain NRD/TAA histories, and use the same cache
    // query policy for current shading and previous-seed confidence retracing.
    configure("denoiser", "reblur-sh"); configure("taa", "1");
    enclosure = sides; enclosure.insert(enclosure.end(), opening.begin(), opening.end());
    capture(std::string(primary) + "-temporal", 96);
    const auto historyResets = temporal().historyResets;
    resets = cache().resets;
    for (int i = 0; i < 24; ++i) {
      environment.radiance.x = .25f + .001f * i;
      frame();
      require(temporal().historyResets == historyResets && cache().resets == resets,
              "Smooth sky changes continuously reset temporal/cache history");
    }
    enclosure.push_back(mover); moverOffset = 3;
    for (int i = 0; i < 64; ++i) frame();
    resets = cache().resets;
    moverOffset = 0;
    for (int i = 0; i < 128; ++i) frame();
    auto blocked = readback(std::string(primary) + "-moving-occluder");
    require(blocked[2] < .015f, "Moving occluder left stale cached illumination");
    require(cache().resets == resets && temporal().historyResets == historyResets,
            "Moving occluder reset shared history");
    moverOffset = 3;
    environment.enabled = 0;
    for (int i = 0; i < 128; ++i) frame();
    auto dark = readback(std::string(primary) + "-sky-disabled");
    require(dark[2] < .01f, "SHARC retained disabled sky illumination");
    require(temporal().historyResets == historyResets, "Sky disable reset TAA/NRD history");
    environment.enabled = 1;
    configure("bounces", "0");
    for (int i = 0; i < 64; ++i) frame();
    auto zero = readback(std::string(primary) + "-zero-bounce");
    require(!cache().active && zero[2] < .01f, "Zero bounces retained cache lighting");
    configure("bounces", "4"); configure("denoiser", "off"); configure("taa", "0");
    configure("sharc", "0"); frame(); frame();
    require(!cache().active, "Runtime cache disable failed");
    configure("sharc", "1"); environment.radiance = {.25f, .5f, .75f};
  }
  // All-scene eligibility is independent of recent query hits or visibility.
  enclosure.clear(); sharpOnly = true;
  configure("denoiser", "reblur-sh"); configure("taa", "1");
  for (int i = 0; i < 12; ++i) frame();
  const auto idleFrames = cache().renderedFrames, idleResets = cache().resets;
  require(cache().active && !cache().updatePaths && !cache().queryAttempts &&
          !cache().occupiedEntries && !cache().gpuTimingsAvailable &&
          cache().gpuUpdateMilliseconds == 0 && cache().gpuResolveMilliseconds == 0,
          "Sharp-only scene retained SHARC update work or stale statistics");
  const auto temporalFrames = temporal().historyFrames;
  for (int i = 0; i < 8; ++i) frame();
  require(cache().renderedFrames == idleFrames && cache().resets == idleResets &&
          temporal().historyFrames > temporalFrames, "Idle cache stopped denoising or continued warming");
  configure("sharcDebug", "1"); frame(); frame(); // No uninitialized cache lookup.
  configure("sharcDebug", "0");
  enclosure.push_back(offscreenReceiver);
  for (int i = 0; i < 12; ++i) frame();
  require(cache().renderedFrames > idleFrames && cache().resets > idleResets && cache().updatePaths,
          "Off-screen eligible receiver did not resume initialized cache updates");
  dynamicOnly = true;
  for (int i = 0; i < 8; ++i) frame();
  require(!cache().updatePaths && !cache().queryAttempts, "Dynamic surfaces kept cache updates alive");
  dynamicOnly = false; sharpOnly = false; enclosure = sides; enclosure.push_back(back);
  for (int i = 0; i < 96; ++i) frame();
  require(cache().updatePaths && cache().occupiedEntries, "Eligible receivers failed to warm after resume");
  if (pbrScene) {
    remixapi_TextureInfo texture{};
    texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO; texture.hash = next++;
    texture.width = texture.height = texture.depth = texture.mipLevels = 1;
    texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    const uint8_t normalPixel[] = {160, 145, 248, 255};
    texture.data = normalPixel; texture.dataSize = sizeof(normalPixel);
    remixapi_TextureHandle normalHandle{}; ok(api.CreateTexture(&texture, &normalHandle));
    char textureName[32]; std::snprintf(textureName, sizeof(textureName), "0x%llx", (unsigned long long)texture.hash);
    const auto normal = std::filesystem::path(textureName);
    const uint8_t roughnessPixel[] = {204, 0, 0, 255};
    texture.hash = next++; texture.data = roughnessPixel;
    remixapi_TextureHandle roughnessHandle{}; ok(api.CreateTexture(&texture, &roughnessHandle));
    std::snprintf(textureName, sizeof(textureName), "0x%llx", (unsigned long long)texture.hash);
    const auto roughnessMap = std::filesystem::path(textureName);
    struct Case { const char *name; remixapi_Float3D color; float roughness, metal; bool mapped, roughnessMapped; };
    for (const auto &test : std::array<Case, 7>{{
        {"dark", {.005f, .008f, .004f}, .8f, 0, false, false},
        {"saturated", {.7f, .025f, .01f}, .8f, 0, false, false},
        {"rough-metal", {.7f, .5f, .3f}, .8f, 1, false, false},
        {"boundary-metal", {.7f, .5f, .3f}, .5f, 1, false, false},
        {"mapped", {.6f, .4f, .2f}, .7f, 0, true, false},
        {"roughness-map", {.6f, .4f, .2f}, .8f, 0, false, true},
        {"mirror", {.4f, .25f, .1f}, 0, 1, false, false}}}) {
      const auto mat = material(test.color, 1, false, true, test.roughnessMapped ? 0 : test.roughness, test.metal,
                                test.mapped ? normal : std::filesystem::path{},
                                test.roughnessMapped ? roughnessMap : std::filesystem::path{});
      receiverOverride = quad(2, .5f, -2, 2, -2, 2, mat);
      enclosure.clear();
      // The hit surfaces must use the tested material too: primary-only metals
      // would exercise PBR rays querying a legacy/dielectric cache instead.
      const auto wall = test.roughness == 0 ? black : mat;
      for (float sign : {-1.f, 1.f}) {
        enclosure.push_back(quad(0, sign, -1, 1, -1, .5f, wall));
        enclosure.push_back(quad(1, sign, -1, .5f, -1, 1, wall));
      }
      enclosure.push_back(quad(2, -1, -1, -.5f, -1, 1, wall));
      enclosure.push_back(quad(2, -1, .5f, 1, -1, 1, wall));
      enclosure.push_back(quad(2, -1, -.5f, .5f, -1, -.5f, wall));
      enclosure.push_back(quad(2, -1, -.5f, .5f, .5f, 1, wall));
      configure("primaryVisibility", "raster"); configure("bounces", test.roughness == 0 ? "1" : "4");
      configure("denoiser", "off"); configure("taa", "0");
      configure("accumulate", "1");
      const auto reference = capture(std::string(test.name) + "-reference", 96);
      configure("accumulate", "0");
      for (int i = 0; i < 160; ++i) frame();
      std::array<float, 3> actual{};
      uint64_t hits = 0;
      for (int i = 0; i < 8; ++i) {
        const auto value = readback(std::string(test.name) + "-cache-" + std::to_string(i));
        hits += cache().queryHits;
        for (int c = 0; c < 3; ++c) actual[c] += value[c] / 8;
      }
      for (int c = 0; c < 3; ++c)
        require(std::abs(actual[c] - reference[c]) < .01f + .12f * reference[c],
                "PBR cache changed material energy excessively");
      require(test.roughness == 0 ? hits == 0 : hits > 0,
              "PBR footprint failed to distinguish rough from mirror paths");
      std::printf("PBR %s reference %.7f %.7f %.7f cached %.7f %.7f %.7f sampled hits %llu\n",
          test.name, reference[0], reference[1], reference[2], actual[0], actual[1], actual[2],
          (unsigned long long)hits);
    }
  }
  ok(api.Shutdown());
  std::puts("SHARC diffuse reference, occlusion, transmission, fallback, controls and temporal checks passed");
}
