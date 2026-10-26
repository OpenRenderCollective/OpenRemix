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
void ok(remixapi_ErrorCode code) {
  require(code == REMIXAPI_ERROR_CODE_SUCCESS, "Guide-cell API failure");
}
using Image = std::vector<std::array<float, 3>>;
} // namespace
int main(int argc, char **argv) {
  require(argc == 2, "Supply capture directory");
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
  startup.windowWidth = 512;
  startup.windowHeight = 512;
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
    ok(api.SetConfigVariable(
        (std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  config("sharc", "0");
  config("sky", "0");
  config("sigma", "0");
  config("localRange", "16");
  config("localIntensity", "1");
  config("adaptiveLocalSampling", "0");
  uint64_t next = 1;
  auto material = [&](remixapi_Float3D color, float alpha, float emission = 0.f,
                      remixapi_Float3D emissionColor = {1, .5f, .2f}) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = next++;
    info.emissiveIntensity = emission;
    info.emissiveColorConstant = emissionColor;
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  auto white = material({.8f, .8f, .8f}, 1), black = material({0, 0, 0}, 1);
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1,
                  remixapi_MaterialHandle material) {
    remixapi_HardcodedVertex v[4]{};
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 4; ++i) {
      v[i].position[axis] = plane;
      v[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      v[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      v[i].color = 0xffffffff;
    }
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = v;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = material;
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = next++;
    mesh.surfaces_values = &surface;
    mesh.surfaces_count = 1;
    remixapi_MeshHandle result{};
    ok(api.CreateMesh(&mesh, &result));
    return result;
  };
  std::vector<remixapi_MeshHandle> meshes;
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = .04f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  std::array<remixapi_LightHandle, 4> lights{};
  for (auto &handle : lights) {
    light.hash = next++;
    ok(api.CreateLight(&light, &handle));
  }
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    for (auto handle : lights)
      ok(api.DrawLightInstance(handle));
    for (auto mesh : meshes) {
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = reinterpret_cast<uintptr_t>(mesh);
      identity.rasterVisible = 1;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      remixapi_InstanceInfoBlendEXT blend{};
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      blend.pNext = &identity;
      blend.alphaBlendEnabled = 0;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &blend;
      instance.mesh = mesh;
      instance.doubleSided = 1;
      for (int i = 0; i < 3; ++i)
        instance.transform.matrix[i][i] = 1;
      ok(api.DrawInstance(&instance));
    }
    ok(api.Present(nullptr));
  });

  int width = 0, height = 0;
  auto capture = [&](const std::string &name) {
    auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int channels;
    auto *data = stbi_load(file.c_str(), &width, &height, &channels, 4);
    require(data, "Missing guide-cell capture");
    Image image(width * height);
    for (size_t i = 0; i < image.size(); ++i)
      for (unsigned c = 0; c < 3; ++c)
        image[i][c] = data[i * 4 + c] / 255.f;
    stbi_image_free(data);
    return image;
  };
  auto difference = [&](const Image &image, int x) {
    std::array<double, 3> delta{};
    // Average strips on either side of a known world-cell boundary. Subtract
    // the unguided strip difference below to remove the physical gradient.
    for (int y = height / 8; y < height * 7 / 8; ++y)
      for (int dx = 0; dx < 8; ++dx)
        for (unsigned c = 0; c < 3; ++c)
          delta[c] += (image[y * width + x + dx][c] -
                       image[y * width + x - 1 - dx][c]) /
                      (8. * (height * 7 / 8 - height / 8));
    return delta;
  };
  auto energy = [&](const Image &image) {
    std::array<double, 3> mean{};
    for (auto pixel : image)
      for (unsigned c = 0; c < 3; ++c) {
        double v = pixel[c];
        mean[c] +=
            std::pow(v, 2.2) /
            image.size();
      }
    return mean;
  };
  config("bounces", "0");
  config("denoiser", "off");
  config("taa", "0");
  config("localSamples", "1");
  config("emissiveCandidates", "4");
  config("debugView", "6");
  auto wall = quad(2, 4, -8, 8, -8, 8, white);
  auto red = material({0, 0, 0}, 1, 2, {1, .02f, .01f});
  auto blueLight = material({0, 0, 0}, 1, 2, {.01f, .1f, 1});
  auto hidden = material({0, 0, 0}, 1, 8, {.1f, 1, .1f});
  meshes = {wall};
  for (int i = 0; i < 64; ++i) {
    float x = -3.2f + (i % 8) * .3f, y = -1.2f + (i / 8) * .3f;
    meshes.push_back(quad(2, 1, x, x + .25f, y, y + .25f, red));
    meshes.push_back(quad(2, 1, -x - .25f, -x, y, y + .25f, blueLight));
    meshes.push_back(quad(2, -1, x, x + .25f, y, y + .25f, hidden));
  }
  meshes.push_back(
      quad(2, 0, -5, 5, -3, 3,
           black)); // hidden green sources produce occlusion evidence
  // Camera begins beyond the light panels; only the flat receiver is visible.
  for (auto &row : camera.projection)
    for (float &v : row)
      v = 0;
  camera.projection[0][0] = camera.projection[1][1] = 1;
  camera.projection[2][2] = 100.f / 99.95f;
  camera.projection[2][3] = 1;
  camera.projection[3][2] = -5.f / 99.95f;
  camera.view[3][2] = -1.5f;

  for (const char *primary : {"raster", "ray"}) {
    config("primaryVisibility", primary);
    config("debugView", "6");
    config("accumulate", "0");
    config("localSamples", "1");
    std::array<Image, 2> averages;
    for (unsigned guide = 0; guide < 2; ++guide) {
      config("localGuiding", guide ? "1" : "0");
      for (int i = 0; i < 80; ++i)
        frame();
      for (int i = 0; i < 12; ++i) {
        auto image = capture(std::string(primary) + "-guide-" +
                             std::to_string(guide) + "-" + std::to_string(i));
        if (averages[guide].empty())
          averages[guide].resize(image.size());
        for (size_t p = 0; p < image.size(); ++p)
          for (unsigned c = 0; c < 3; ++c)
            averages[guide][p][c] += image[p][c] / 12;
      }
      remixapi_LocalLightGuidingStatisticsEXT g{};
      g.structSize = sizeof(g);
      ok(remixapi_GetLocalLightGuidingStatisticsEXT(&g));
      if (guide)
        require(g.active && g.sampledGuided > g.sampledQueries / 10,
                "Color boundary fixture did not exercise guiding");
    }
    double seam = 0;
    // The camera sees five world units: cell boundaries at X=-2,0,2 lie at
    // 10%, 50%, and 90% of the image. The receiver and illumination are smooth.
    for (float fraction : {.1f, .5f, .9f}) {
      auto reference = difference(averages[0], int(fraction * width));
      auto guided = difference(averages[1], int(fraction * width));
      for (unsigned c = 0; c < 3; ++c)
        seam = std::max(seam, std::abs(guided[c] - reference[c]));
    }
    std::printf("%s: excess display-space guide-cell jump %.6f\n", primary,
                seam);
    require(seam < .02, "Guiding introduced coherent colored cell boundaries");
    // An independent, linear progressive reference verifies that smoothing
    // proposals preserved energy. Average before encoding/clipping; debug
    // presentation deliberately bypasses accumulation, so use final output.
    config("debugView", "0");
    config("accumulate", "1");
    config("localSamples", "4");
    std::array<std::array<double, 3>, 2> means;
    for (unsigned guide = 0; guide < 2; ++guide) {
      config("localGuiding", guide ? "1" : "0");
      ++epoch.historyResetEpoch;
      for (unsigned i = 0; i < 128; ++i)
        frame();
      means[guide] = energy(capture(
          std::string(primary) + "-linear-reference-" + std::to_string(guide)));
    }
    for (unsigned c = 0; c < 3; ++c) {
      require(means[0][c] > .001, "Reference lost a colored source");
      require(std::abs(means[1][c] - means[0][c]) < means[0][c] * .03,
              "Guide interpolation changed linear lighting energy");
    }
  }
  ok(api.Shutdown());
}
