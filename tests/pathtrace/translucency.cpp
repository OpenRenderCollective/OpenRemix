#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <sstream>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
void ok(remixapi_ErrorCode error,
        std::source_location where = std::source_location::current()) {
  if (error) {
    std::fprintf(stderr, "Translucency API %x at line %u\n", unsigned(error),
                 where.line());
    std::exit(1);
  }
}
using Image = std::vector<float>;
Image load(const std::filesystem::path &path, unsigned &width, unsigned &height,
           bool signedValues = false) {
  std::ifstream input(path, std::ios::binary);
  std::string magic;
  float scale;
  input >> magic >> width >> height >> scale;
  input.get();
  require(input.good() && magic == "PF" && scale == -1,
          "Missing linear translucency capture");
  Image pixels(size_t(width) * height * 3);
  input.read(reinterpret_cast<char *>(pixels.data()),
             pixels.size() * sizeof(float));
  require(input.good(), "Incomplete translucency capture");
  for (float value : pixels)
    require(std::isfinite(value) && (signedValues || value >= 0),
            "Invalid translucent radiance");
  return pixels;
}
} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "Supply capture directory");
  auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  SDL_setenv_unsafe("REMIX_PROFILE_EVERY_FRAME", "1", 1);
  const bool reuseTest =
      std::getenv("REMIX_TEST_TRANSLUCENCY_REUSE") != nullptr;
  const auto profile = directory / "rays.csv";
  if (reuseTest) {
    std::filesystem::remove(profile);
    SDL_setenv_unsafe("REMIX_RAY_PROFILE", profile.string().c_str(), 1);
  }
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.windowWidth = 95;
  startup.windowHeight = 79;
  ok(api.Startup(&startup));
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
  for (const char *key :
       {"autoExposure", "bloom", "taa", "sigma", "sharc",
        "adaptiveLocalSampling", "blueNoiseLocalLights", "waterIndirectReuse",
        "translucencyReuse"})
    config(key, "0");
  config("upscaler", "off");
  config("denoiser", "off");
  config("bounces", "1");
  config("localSamples", "1");
  config("primaryVisibility",
         std::getenv("REMIX_TEST_RAY_PRIMARY") ? "ray" : "raster");
  uint64_t id = 100;
  auto material = [&](remixapi_Float3D color, float alpha, float roughness) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.roughnessConstant = roughness;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = id++;
    remixapi_MaterialHandle handle{};
    ok(api.CreateMaterial(&info, &handle));
    return handle;
  };
  auto mesh = [&](remixapi_MaterialHandle mat, float size, float slope = 0) {
    remixapi_HardcodedVertex vertices[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      vertices[i].position[0] = i == 0 || i == 3 ? -size : size;
      vertices[i].position[1] = i < 2 ? -size : size;
      vertices[i].position[2] = slope * vertices[i].position[0];
      vertices[i].color = 0xffffffff;
    }
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = mat;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = id++;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle handle{};
    ok(api.CreateMesh(&info, &handle));
    return handle;
  };
  auto background = mesh(
      material(reuseTest ? remixapi_Float3D{.4f, .4f, .4f} : remixapi_Float3D{},
               1, 0),
      2);
  auto water = mesh(material({.15f, .55f, .8f}, .6f, reuseTest ? 0 : .75f), .8f,
                    reuseTest ? 1.5f : 0);
  std::array<remixapi_LightHandle, 3> lights{};
  for (unsigned i = 0; i < lights.size(); ++i) {
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {float(i) * .5f - .5f, i == 1 ? .3f : -.2f, -.5f};
    sphere.radius = .1f;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = id++;
    info.radiance = {i == 0 ? 8.f : 1.f, i == 1 ? 8.f : 1.f,
                     i == 2 ? 8.f : 1.f};
    ok(api.CreateLight(&info, &lights[i]));
  }
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, 0, 1};
  remixapi_LightInfo sunInfo{};
  sunInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  sunInfo.pNext = &distant;
  sunInfo.hash = id++;
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&sunInfo, &sun));
  remixapi_CameraEnvironmentEXT sky{};
  sky.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  sky.enabled = 1;
  sky.radiance = {.02f, .04f, .08f};
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1;
  epoch.pNext = &sky;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  float shift = 0;
  unsigned layers = 1;
  bool lit = true;
  auto draw = [&](remixapi_MeshHandle geometry, uint64_t identity, float x,
                  float z) {
    remixapi_InstanceIdentityEXT stable{};
    stable.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    stable.instanceId = identity;
    stable.rasterVisible = 1;
    remixapi_InstanceInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    info.pNext = &stable;
    info.mesh = geometry;
    info.doubleSided = 1;
    for (int i = 0; i < 3; ++i)
      info.transform.matrix[i][i] = 1;
    info.transform.matrix[0][3] = x;
    info.transform.matrix[2][3] = z;
    ok(api.DrawInstance(&info));
  };
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    if (lit)
      for (auto light : lights)
        ok(api.DrawLightInstance(light));
    draw(background, 1, 0, .9f);
    for (unsigned i = 0; i < layers; ++i)
      draw(water, 2 + i, shift, .2f + .1f * i);
    ok(api.Present(nullptr));
  });
  auto warm = [&](unsigned count) {
    for (unsigned i = 0; i < count; ++i)
      frame();
  };
  unsigned width = 0, height = 0;
  auto capture = [&](const std::string &name, const char *signal = nullptr) {
    auto path = directory / (name + ".pfm");
    SDL_setenv_unsafe("REMIX_PATH_LINEAR_CAPTURE", path.string().c_str(), 1);
    if (signal)
      SDL_setenv_unsafe("REMIX_PATH_LINEAR_SIGNAL", signal, 1);
    frame();
    SDL_unsetenv_unsafe("REMIX_PATH_LINEAR_CAPTURE");
    SDL_unsetenv_unsafe("REMIX_PATH_LINEAR_SIGNAL");
    warm(3);
    return load(path, width, height, signal != nullptr);
  };
  auto mean = [&](const Image &image) {
    double sum = 0;
    unsigned count = 0;
    for (unsigned y = height / 4; y < 3 * height / 4; ++y)
      for (unsigned x = width / 4; x < 3 * width / 4; ++x)
        for (unsigned c = 0; c < 3; ++c) {
          sum += image[(y * width + x) * 3 + c];
          ++count;
        }
    return sum / count;
  };
  auto mse = [&](const Image &a, const Image &b) {
    Image delta(a.size());
    for (size_t i = 0; i < a.size(); ++i)
      delta[i] = (a[i] - b[i]) * (a[i] - b[i]);
    return mean(delta);
  };
  if (reuseTest) {
    // The tilted sheet reflects toward an opaque receiver that is also visible
    // in the current camera. This exercises accepted screen-space endpoints.
    config("samples", "16");
    config("accumulate", "1");
    warm(96);
    const auto reference = capture("reference");
    config("accumulate", "0");
    config("samples", "1");
    config("denoiser", "reblur-sh");
    auto now = [] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    };
    std::array<double, 2> rays{}, energy{}, error{};
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
      config("translucencyReuse", enabled ? "1" : "0");
      ++epoch.worldEpoch;
      warm(64);
      const auto begin = now();
      warm(64);
      const auto end = now();
      for (unsigned i = 0; i < 12; ++i) {
        auto image = capture("reuse-" + std::to_string(enabled) + "-" +
                             std::to_string(i));
        energy[enabled] += mean(image) / 12;
        error[enabled] += mse(image, reference) / 12;
      }
      std::ifstream input(profile);
      auto split = [](const std::string &line) {
        std::vector<std::string> values;
        std::istringstream stream(line);
        for (std::string value; std::getline(stream, value, ',');)
          values.push_back(value);
        return values;
      };
      std::string line;
      std::getline(input, line);
      auto header = split(line);
      auto query =
          std::find(header.begin(), header.end(), "transparency_local_queries");
      require(query != header.end(), "Missing translucency ray profile");
      const auto column = size_t(query - header.begin());
      unsigned frames = 0;
      while (std::getline(input, line)) {
        auto values = split(line);
        if (values.size() <= column)
          continue;
        const auto time = std::stoll(values[0]);
        if (time >= begin && time <= end) {
          rays[enabled] += std::stod(values[column]);
          ++frames;
        }
      }
      require(frames >= 32, "Incomplete reuse ray measurement");
      rays[enabled] /= frames;
    }
    std::printf("opaque reuse rays off=%.3f on=%.3f energy off=%.6f on=%.6f "
                "reference=%.6f mse off=%.9f on=%.9f\n",
                rays[0], rays[1], energy[0], energy[1], mean(reference),
                error[0], error[1]);
    require(rays[0] > 0 && rays[1] < rays[0] * .98,
            "Checked opaque reuse did not avoid endpoint lighting rays");
    require(std::abs(energy[1] / mean(reference) - 1) < .15,
            "Checked opaque reuse changed reference lighting energy");
    require(error[1] < error[0] * 1.5 + .00001,
            "Checked opaque reuse increased reference error excessively");
    ok(api.Shutdown());
    return 0;
  }
  config("samples", "16");
  config("accumulate", "1");
  warm(96);
  const auto reference = capture("reference");
  require(mean(reference) > .002, "Reference water is unlit");
  config("accumulate", "0");
  config("samples", "1");
  config("denoiser", "reblur-sh");
  config("translucencyDenoiser", "0");
  warm(40);
  double rawError = 0;
  for (unsigned i = 0; i < 12; ++i)
    rawError += mse(capture("unfiltered-" + std::to_string(i)), reference) / 12;
  remixapi_DenoisingStatisticsEXT before{};
  before.structSize = sizeof(before);
  ok(remixapi_GetDenoisingStatisticsEXT(&before));
  config("translucencyDenoiser", "1");
  warm(64);
  remixapi_DenoisingStatisticsEXT after{};
  after.structSize = sizeof(after);
  ok(remixapi_GetDenoisingStatisticsEXT(&after));
  require(after.historyResets == before.historyResets,
          "Foreground toggle reset opaque history");
  double filteredError = 0, filteredMean = 0;
  for (unsigned i = 0; i < 12; ++i) {
    auto image = capture("filtered-" + std::to_string(i));
    filteredError += mse(image, reference) / 12;
    filteredMean += mean(image) / 12;
  }
  std::printf("foreground raw_mse=%.9f filtered_mse=%.9f reference_mean=%.6f "
              "filtered_mean=%.6f\n",
              rawError, filteredError, mean(reference), filteredMean);
  require(filteredError < rawError * .5,
          "Foreground denoising did not reduce error");
  require(std::abs(filteredMean / mean(reference) - 1) < .15,
          "Foreground denoising changed lighting energy");
  if (std::getenv("REMIX_TEST_FOREGROUND_SIGNALS")) {
    for (const char *signal :
         {"front-diffuse", "front-specular", "front-diffuse-output",
          "front-specular-output"}) {
      auto image = capture(signal, signal);
      std::printf("foreground signal %s mean %.9f\n", signal, mean(image));
    }
  }
  shift += .02f;
  auto motion = capture("front-motion", "front-motion");
  const size_t center = ((height / 2) * width + width / 2) * 3;
  require(std::abs(motion[center] + float(width) * .01f) < .03f &&
              std::abs(motion[center + 1]) < .001f,
          "Foreground motion followed the opaque receiver");
  // Motion changes the foreground position while the opaque receiver stays
  // fixed.
  for (unsigned i = 0; i < 20; ++i) {
    shift += .02f;
    frame();
  }
  auto moving = capture("moving");
  double trail = 0;
  unsigned count = 0;
  for (unsigned y = height / 3; y < 2 * height / 3; ++y)
    for (unsigned x = width / 8; x < width / 4; ++x)
      for (unsigned c = 0; c < 3; ++c) {
        trail += moving[(y * width + x) * 3 + c];
        ++count;
      }
  require(trail / count < mean(reference) * .02,
          "Water left a disocclusion trail");
  // Explicit reset and a disappearance must not preserve foreground radiance.
  ++epoch.worldEpoch;
  layers = 0;
  warm(3);
  require(mean(capture("removed")) < .00001,
          "Removed water retained foreground history");
  layers = 2;
  shift = 0;
  warm(16);
  require(mean(capture("overlap")) > .001, "Layered fallback disappeared");
  layers = 1;
  lit = false;
  sky.radiance = {0, 0, 0};
  ++epoch.worldEpoch;
  warm(4);
  require(mean(capture("reset-dark")) < .00001,
          "World reset retained old water lighting");
  ok(api.Shutdown());
}
