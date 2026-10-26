#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <remix/remix_native_ext.h>
#include <remix/remix_reconstruction_ext.h>
#include <source_location>
#include <string>
#include <vector>

namespace {
  void require(bool b, const char *message) {
    if (!b) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void ok(remixapi_ErrorCode code, std::source_location where = std::source_location::current()) {
    if (code != REMIXAPI_ERROR_CODE_SUCCESS) {
      std::fprintf(stderr, "API failure %d at line %u\n", int(code), where.line());
      std::exit(1);
    }
  }
  using Image = std::vector<float>;
  Image load(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::string magic;
    unsigned w, h;
    float scale;
    file >> magic >> w >> h >> scale;
    file.get();
    require(file.good() && magic == "PF" && scale == -1, "Missing linear HDR capture");
    Image image(size_t(w) * h * 3);
    file.read(reinterpret_cast<char *>(image.data()), image.size() * sizeof(float));
    require(file.good(), "Incomplete HDR capture");
    for (float v : image)
      require(std::isfinite(v) && v >= 0, "Nonfinite/negative radiance");
    return image;
  }
} // namespace
int main(int argc, char **argv) {
  require(argc == 2, "Supply report directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  const bool benchmark = std::getenv("REMIX_TEST_FIREFLY_BENCHMARK");
  const bool baseline = std::getenv("REMIX_TEST_FIREFLY_BASELINE");
  auto option = [](const char *key, const char *fallback) {
    const auto *value = std::getenv(key);
    return value ? value : fallback;
  };
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = benchmark ? 1920 : 96;
  startup.windowHeight = benchmark ? 1080 : 80;
#endif
  ok(api.Startup(&startup));
  // These fixtures measure the unexposed lighting/display contract.
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
  if (benchmark) {
    remixapi_WindowMetrics metrics{};
    ok(remixapi_GetWindowMetrics(&metrics));
    require(metrics.drawableWidth == 1920 && metrics.drawableHeight == 1080,
            "Benchmark requires native 1920x1080");
  }
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2)) {
    ok(api.Shutdown());
    return 77;
  }
  bool progressive = false;
  auto config = [&](const char *key, const char *value) {
    if (std::string(key) == "accumulate")
      progressive = std::string(value) == "1";
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  auto invalid = [&](const char *key, const char *value) {
    require(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value) ==
                REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "Accepted invalid firefly option");
  };
  if (!baseline) {
    for (const char *key : {"localDirectClamp", "indirectClamp"}) {
      for (const char *value : {"-1", "1000001", "nan", "inf", "1junk", "", "1e40"})
        invalid(key, value);
      for (const char *value : {"0", "0.001", "1e6"})
        config(key, value);
      config(key, "0");
    }
    for (const char *value : {"2", "yes", "-1"})
      invalid("localFireflyFilter", value);
    for (const char *value : {"2", "-1", "yes", "", "nan", "1junk"})
      invalid("russianRoulette", value);
    for (const char *value : {"0", "5", "-1", "1.5", "inf", "1junk"})
      invalid("rouletteMinBounces", value);
  }
  config("upscaler", "off");
  config("sharc", "0");
  config("sharcCapacityLog2", "16");
  config("sky", "0");
  config("sigma", "0");
  config("denoiser", "off");
  config("taa", "0");
  config("localGuiding", "0");
  config("adaptiveLocalSampling", "0");
  config("blueNoiseLocalLights", "0");
  config("localRange", "16");
  config("localSamples", "4");
  config("samples", "16");
  config("primaryVisibility", "ray");
  config("emissiveCandidates", "8");
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  uint64_t next = 1;
  // Linear data maps exercise the same texture path as game PBR assets.
  const std::array<uint8_t, 16> pixels = {186, 128, 240, 255, 70,  155, 240, 255,
                                          70,  155, 240, 255, 186, 128, 240, 255};
  remixapi_TextureInfo texture{};
  texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  texture.hash = 0xff00;
  texture.width = texture.height = 2;
  texture.depth = texture.mipLevels = 1;
  texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  texture.data = pixels.data();
  texture.dataSize = pixels.size();
  remixapi_TextureHandle normal{};
  ok(api.CreateTexture(&texture, &normal));
  const auto normalName = std::filesystem::path("0xff00");
  const bool maps = std::string(option("REMIX_TEST_FIREFLY_MAPS", "1")) == "1";
  const bool pbrEnabled = std::string(option("REMIX_TEST_FIREFLY_PBR", "1")) == "1";
  auto material = [&](remixapi_Float3D color, float roughness, float metal, float emission = 0.f,
                      float alpha = 1.f, bool enablePbr = true) {
    remixapi_MaterialInfoPbrEXT pbr{};
    pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    pbr.enabled = enablePbr && pbrEnabled;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &pbr;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = 7;
    opaque.roughnessConstant = roughness;
    opaque.metallicConstant = metal;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    if (maps && !emission)
      info.normalTexture = normalName.c_str();
    info.hash = next++;
    info.emissiveIntensity = emission;
    info.emissiveColorConstant = color;
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  auto white = material({.65f, .65f, .65f}, .7f, 0);
  auto emission = material({.25f, .5f, 1}, 1, 0, 80);
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1,
                  remixapi_MaterialHandle mat) {
    remixapi_HardcodedVertex vertices[4]{};
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 4; ++i) {
      vertices[i].position[axis] = plane;
      vertices[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      vertices[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      vertices[i].texcoord[0] = i == 0 || i == 3 ? 0 : 1;
      vertices[i].texcoord[1] = i < 2 ? 0 : 1;
      vertices[i].color = 0xffffffff;
    }
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = mat;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = next++;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle result{};
    ok(api.CreateMesh(&info, &result));
    return result;
  };
  std::vector<remixapi_MeshHandle> meshes;
  const float roughness[] = {.03f, .08f, .2f, .5f, .85f};
  const remixapi_Float3D colors[] = {
      {.65f, .65f, .65f}, {.008f, .012f, .006f}, {.9f, .005f, .02f}, {.8f, .3f, .05f}};
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 5; ++x)
      meshes.push_back(quad(2, 4, -2 + x * .8f, -1.2f + x * .8f, -2 + y, -1 + y,
                            material(colors[y], roughness[x], y == 3 ? 1.f : 0.f)));
  for (auto mesh :
       {quad(1, -2, 0, 4, -2, 2, white), quad(1, 2, 0, 4, -2, 2, white), quad(0, -2, -2, 2, 0, 4, white),
        quad(0, 2, -2, 2, 0, 4, white), quad(2, 0, -2, 2, -2, 2, white)})
    meshes.push_back(mesh);
  const size_t emitterBegin = meshes.size();
  for (int i = 0; i < std::stoi(option("REMIX_TEST_FIREFLY_EMITTER_COUNT", "8")); ++i)
    meshes.push_back(quad(2, 2.2f, -.9f + i * .25f, -.8f + i * .25f, -.8f, -.7f, emission));
  const size_t emitterEnd = meshes.size();
  std::vector<remixapi_LightHandle> lights;
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = .08f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  for (int i = 0; i < std::stoi(option("REMIX_TEST_FIREFLY_LIGHT_COUNT", "16")); ++i) {
    sphere.position = {-.9f + .6f * (i % 4), -.9f + .6f * (i / 4), 2};
    light.radiance = {60.f + 20.f * (i % 3), 40.f, 20.f};
    light.hash = next++;
    remixapi_LightHandle result{};
    ok(api.CreateLight(&light, &result));
    lights.push_back(result);
  }
  remixapi_LightInfoDistantEXT sunInfo{};
  sunInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  sunInfo.direction = {0, -1, 0};
  remixapi_LightInfo zeroSun{};
  zeroSun.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  zeroSun.pNext = &sunInfo;
  zeroSun.hash = next++;
  if (std::getenv("REMIX_TEST_FIREFLY_SUN")) {
    sunInfo.direction = {.3f, .3f, 1.f};
    zeroSun.radiance = {2.f, 3.f, 4.f};
  }
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&zeroSun, &sun));
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1;
  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1;
  environment.radiance = {.5f, .8f, 1.f};
  epoch.pNext = &environment;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &epoch;
  for (unsigned i = 0; i < 4; ++i)
    camera.view[i][i] = 1;
  camera.projection[0][0] = camera.projection[1][1] = 1;
  camera.projection[2][2] = 100.f / 99.95f;
  camera.projection[2][3] = 1;
  camera.projection[3][2] = -5.f / 99.95f;
  camera.view[3][2] = -1;
  size_t transparentIndex = SIZE_MAX;
  if (std::getenv("REMIX_TEST_FIREFLY_TRANSPARENT")) {
    transparentIndex = meshes.size();
    meshes.push_back(quad(2, 2.8f, -2, 2, -2, 2,
                          material({.3f, .65f, .9f}, .08f, .2f, 0, .4f,
                                   std::string(option("REMIX_TEST_FIREFLY_TRANSPARENT", "1")) != "2")));
  }
  const std::string family = option("REMIX_TEST_FIREFLY_FAMILY", "mixed");
  bool removeLights = false;
  const std::string motion = option("REMIX_TEST_FIREFLY_MOTION", "off");
  unsigned serial = 0;
  float blockerOffset = 0;
  auto submit = [&] {
    ok(api.PumpEvents());
    if (!progressive)
      sunInfo.direction.x = (++serial & 1) ? .001f : 0;
    ok(api.UpdateLightDefinition(sun, &zeroSun));
    ok(api.DrawLightInstance(sun));

    if (motion == "camera")
      camera.view[3][0] = .03f * std::sin(float(serial) * .1f);
    if (motion == "light") {
      sphere.position.x = .8f * std::sin(float(serial) * .1f);
      ok(api.UpdateLightDefinition(lights[0], &light));
    }
    ok(api.SetupCamera(&camera));
    if (family != "emissive" && !removeLights)
      for (auto handle : lights)
        ok(api.DrawLightInstance(handle));
    for (size_t i = 0; i < meshes.size(); ++i) {
      if (i == 24 && (std::string(option("REMIX_TEST_FIREFLY_SKY", "0")) == "1" ||
                      std::getenv("REMIX_TEST_FIREFLY_SUN")))
        continue;
      if (i >= emitterBegin && i < emitterEnd && (family == "torch" || removeLights))
        continue;
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = i + 1;
      identity.rasterVisible = 1;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &identity;
      remixapi_InstanceInfoBlendEXT blend{};
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      if (i == transparentIndex) {
        blend.pNext = &identity;
        blend.alphaBlendEnabled = 1;
        instance.pNext = &blend;
      }
      instance.mesh = meshes[i];
      instance.doubleSided = 1;
      for (unsigned j = 0; j < 3; ++j)
        instance.transform.matrix[j][j] = 1;
      if (i == 7)
        instance.transform.matrix[2][3] = blockerOffset;
      ok(api.DrawInstance(&instance));
    }
    ok(api.Present(nullptr));
  };
  auto frame = remix::test::preparedFrame(submit);
  auto capture = [&](const std::string &name) {
    auto path = directory / (name + ".pfm");
    SDL_setenv_unsafe("REMIX_PATH_LINEAR_CAPTURE", path.string().c_str(), 1);
    frame();
    SDL_unsetenv_unsafe("REMIX_PATH_LINEAR_CAPTURE");
    frame();
    frame();
    frame();
    return load(path);
  };
  for (const auto &[key, env, fallback] : std::initializer_list<std::array<const char *, 3>>{
           {"emissiveCandidates", "REMIX_TEST_FIREFLY_CANDIDATES", "8"},
           {"samples", "REMIX_TEST_FIREFLY_SAMPLES", "1"},
           {"localSamples", "REMIX_TEST_FIREFLY_LOCAL_SAMPLES", "1"},
           {"bounces", "REMIX_TEST_FIREFLY_BOUNCES", "2"},
           {"primaryVisibility", "REMIX_TEST_FIREFLY_PRIMARY", "raster"},
           {"denoiser", "REMIX_TEST_FIREFLY_DENOISER", "off"},
           {"taa", "REMIX_TEST_FIREFLY_TAA", "0"},
           {"sharc", "REMIX_TEST_FIREFLY_SHARC", "0"},
           {"separateLocalLighting", "REMIX_TEST_FIREFLY_SPLIT", "0"},
           {"localLights", "REMIX_TEST_FIREFLY_LOCAL", "1"},
           {"localDenoiser", "REMIX_TEST_FIREFLY_LOCAL_DENOISER", "1"},
           {"sky", "REMIX_TEST_FIREFLY_SKY", "0"},
           {"accumulate", "REMIX_TEST_FIREFLY_ACCUMULATE", "0"},
           {"upscaler", "REMIX_TEST_FIREFLY_UPSCALER", "off"},
           {"dlssRayReconstruction", "REMIX_TEST_FIREFLY_RR", "0"},
           {"pbrLocalReuse", "REMIX_TEST_FIREFLY_REUSE", "0"},
           {"waterIndirectReuse", "REMIX_TEST_FIREFLY_WATER", "1"}})
    config(key, option(env, fallback));
  config("dlssQuality", "native-aa");
  if (!baseline) {
    config("russianRoulette", option("REMIX_TEST_FIREFLY_ROULETTE", "0"));
    config("rouletteMinBounces", option("REMIX_TEST_FIREFLY_ROULETTE_MIN", "1"));
    config("localDirectClamp", option("REMIX_TEST_FIREFLY_DIRECT", "0"));
    config("indirectClamp", option("REMIX_TEST_FIREFLY_INDIRECT", "0"));
    config("localFireflyFilter", option("REMIX_TEST_FIREFLY_FILTER", "0"));
  }
  if (benchmark) {
    // No captures or ray/stage instrumentation during timing.
    require(!std::getenv("REMIX_PATH_LINEAR_CAPTURE") && !std::getenv("REMIX_RAY_PROFILE") &&
                !std::getenv("REMIX_STACK_PROFILE"),
            "Instrumentation enabled during benchmark");
    for (unsigned i = 0; i < 16; ++i)
      frame(); // Populate pipelines and retire timestamp queries before warmup.
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until)
      frame();
    std::ofstream csv(directory / "timing.csv");
    csv << "trace_ms,denoise_ms,confidence_ms,taa_ms\n";
    until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < until) {
      frame();
      remixapi_TracingStatisticsEXT t{};
      t.structSize = sizeof(t);
      remixapi_DenoisingStatisticsEXT d{};
      d.structSize = sizeof(d);
      ok(remixapi_GetTracingStatisticsEXT(&t));
      ok(remixapi_GetDenoisingStatisticsEXT(&d));
      require(t.gpuTimingsAvailable, "GPU timing unavailable");
      csv << t.gpuTraceMilliseconds << ',' << d.gpuDenoiseMilliseconds << ',' << d.gpuConfidenceMilliseconds
          << ',' << d.gpuTaaMilliseconds << '\n';
    }
  } else {
    const unsigned warmup = std::stoul(option("REMIX_TEST_FIREFLY_WARMUP", "16"));
    for (unsigned i = 0; i < warmup; ++i)
      frame();
    const unsigned count = std::stoul(option("REMIX_TEST_FIREFLY_FRAMES", "8"));
    const unsigned averaging = std::stoul(option("REMIX_TEST_FIREFLY_AVERAGE", "0"));
    if (averaging) {
      std::vector<double> sum;
      for (unsigned i = 0; i < averaging; ++i) {
        auto sample = capture("reference-latest");
        if (sum.empty())
          sum.resize(sample.size());
        for (size_t j = 0; j < sum.size(); ++j)
          sum[j] += sample[j];
      }
      std::ifstream header(directory / "reference-latest.pfm", std::ios::binary);
      std::string magic;
      unsigned width, height;
      float scale;
      header >> magic >> width >> height >> scale;
      std::ofstream mean(directory / "frame-0.pfm", std::ios::binary);
      mean << "PF\n" << width << ' ' << height << "\n-1.0\n";
      for (double v : sum) {
        float value = float(v / averaging);
        mean.write(reinterpret_cast<const char *>(&value), sizeof(value));
      }
    } else
      for (unsigned i = 0; i < count; ++i) {
        if (motion == "remove" && i == count / 2)
          removeLights = true;
        auto sample = capture("frame-" + std::to_string(i));
        const float directLimit = std::stof(option("REMIX_TEST_FIREFLY_DIRECT", "0"));
        const float indirectLimit = std::stof(option("REMIX_TEST_FIREFLY_INDIRECT", "0"));
        const bool noBounces = std::string(option("REMIX_TEST_FIREFLY_BOUNCES", "2")) == "0";
        if (directLimit > 0 && (noBounces || indirectLimit > 0) &&
            std::string(option("REMIX_TEST_FIREFLY_DENOISER", "off")) == "off" &&
            !std::getenv("REMIX_TEST_FIREFLY_TRANSPARENT") && !std::getenv("REMIX_TEST_FIREFLY_SUN") &&
            !std::getenv("REMIX_EMISSIVE_HIT_DISTANCE")) {
          // Back-wall interior excludes directly visible emitter quads.
          const float limit = directLimit + (noBounces ? 0 : indirectLimit);
          for (unsigned y = 14; y < 60; ++y)
            for (unsigned x = 18; x < 78; ++x) {
              const auto offset = ((79 - y) * 96 + x) * 3;
              const float luminance =
                  .2126f * sample[offset] + .7152f * sample[offset + 1] + .0722f * sample[offset + 2];
              require(luminance <= limit * 1.0001f + 1e-6f, "Outgoing sample exceeded clamp threshold");
            }
        }
      }
    remixapi_TracingStatisticsEXT t{};
    t.structSize = sizeof(t);
    ok(remixapi_GetTracingStatisticsEXT(&t));
    remixapi_DenoisingStatisticsEXT d{};
    d.structSize = sizeof(d);
    ok(remixapi_GetDenoisingStatisticsEXT(&d));
    remixapi_ReconstructionDiagnosticsEXT r{};
    r.structSize = sizeof(r);
    ok(remixapi_GetReconstructionDiagnosticsEXT(&r));
    remixapi_SharcStatisticsEXT sharc{};
    sharc.structSize = sizeof(sharc);
    ok(remixapi_GetSharcStatisticsEXT(&sharc));
    std::ofstream metadata(directory / "state.txt");
    metadata << "samples=" << t.samplesPerPixel << " bounces=" << t.indirectBounces
             << " accumulation=" << t.accumulationCount << " sequence=" << t.sampleSequence
             << " reblur=" << d.activeDenoiser << " taa=" << d.activeTaa << " sharc=" << sharc.active
             << " sharc_entries=" << sharc.occupiedEntries << " sharc_updates=" << sharc.updatePaths
             << " sharc_queries=" << sharc.queryAttempts << " sharc_hits=" << sharc.queryHits
             << " sharc_memory=" << sharc.memoryBytes
             << " reconstruction=" << r.activeBackend << " reconstruction_capabilities=" << r.capabilities
             << " reconstruction_frames=" << r.evaluatedFrames << " reconstruction_failures=" << r.failures
             << '\n';
    metadata << "reconstruction_reason=" << r.fallbackReason << '\n';
    if (std::getenv("REMIX_TEST_ROULETTE_RESETS") && !baseline) {
      config("bounces", "4");
      config("russianRoulette", "0");
      config("rouletteMinBounces", "1");
      for (const char *accumulate : {"1", "0"}) {
        config("accumulate", accumulate);
        config("denoiser", *accumulate == '1' ? "off" : "reblur-sh");
        config("taa", *accumulate == '1' ? "0" : "1");
        for (const auto &[key, value] : std::initializer_list<std::pair<const char *, const char *>>{
                 {"russianRoulette", "1"}, {"rouletteMinBounces", "2"},
                 {"rouletteMinBounces", "4"}, {"russianRoulette", "0"}, {"rouletteMinBounces", "1"}}) {
          for (unsigned i = 0; i < 8; ++i) frame();
          ok(remixapi_GetDenoisingStatisticsEXT(&d));
          const auto resets = d.historyResets;
          config(key, value);
          // Observe the reset immediately; optional pipeline preparation can
          // present many subsequent progressive samples while frame() waits.
          submit();
          ok(remixapi_GetTracingStatisticsEXT(&t));
          if (*accumulate == '1')
            require(t.accumulationCount <= 2, "Adaptive control did not reset accumulation");
          else {
            for (unsigned i = 0; i < 4; ++i) frame();
            ok(remixapi_GetDenoisingStatisticsEXT(&d));
            require(d.historyResets > resets, "Adaptive control did not reset temporal history");
          }
        }
      }
    }
    if (std::getenv("REMIX_TEST_FIREFLY_RESETS") && !baseline) {
      config("accumulate", "1");
      for (unsigned i = 0; i < 5; ++i)
        frame();
      ok(remixapi_GetTracingStatisticsEXT(&t));
      require(t.accumulationCount >= 4, "Accumulation did not grow");
      for (const auto *key : {"localDirectClamp", "indirectClamp", "localFireflyFilter"}) {
        config(key, "1");
        frame();
        ok(remixapi_GetTracingStatisticsEXT(&t));
        require(t.accumulationCount <= 2, "Clamp/filter did not reset progressive accumulation");
        for (unsigned i = 0; i < 5; ++i)
          frame();
      }
      config("accumulate", "0");
      config("denoiser", "reblur-sh");
      config("taa", "1");
      for (unsigned i = 0; i < 10; ++i)
        frame();
      ok(remixapi_GetDenoisingStatisticsEXT(&d));
      for (const auto *key : {"localDirectClamp", "indirectClamp", "localFireflyFilter"}) {
        auto resets = d.historyResets;
        config(key, "0");
        for (unsigned i = 0; i < 5; ++i)
          frame();
        ok(remixapi_GetDenoisingStatisticsEXT(&d));
        require(d.historyResets > resets, "Clamp/filter did not reset temporal history");
      }
    }
  }
  ok(api.Shutdown());
  return 0;
}
