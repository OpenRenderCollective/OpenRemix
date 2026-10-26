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
  require(code == REMIXAPI_ERROR_CODE_SUCCESS, "Adaptive sampling API failure");
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
  startup.windowWidth = 256;
  startup.windowHeight = 256;
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
  uint64_t next = 1;
  auto material = [&](remixapi_Float3D color, float alpha, float emission = 0) {
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
    info.emissiveColorConstant = {1, 1, 1};
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  auto white = material({.8f, .8f, .8f}, 1);
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
  auto receiver = quad(2, 192.f, 191.f, 193.f, 63.f, 67.f, white);
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
  sphere.radius = .05f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  std::array<remixapi_LightHandle, 4> lights{};
  for (auto &handle : lights) {
    light.hash = next++;
    ok(api.CreateLight(&light, &handle));
  }
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, -1, 0};
  remixapi_LightInfo zeroSun{};
  zeroSun.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  zeroSun.pNext = &distant;
  zeroSun.hash = next++;
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&zeroSun, &sun));
  unsigned renderSequence = 0;
  auto frame = remix::test::preparedFrame([&] {
    // Force fresh frames with an unlit sun; the camera and torch scene stay
    // fixed.
    distant.direction.x = (++renderSequence & 1) ? .001f : 0;
    ok(api.UpdateLightDefinition(sun, &zeroSun));
    ok(api.DrawLightInstance(sun));
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
  // Inspect the production budget through the existing raw diagnostic output.
  // A broad, dim cluster and a compact, bright cluster have nearly equal BVH
  // proposal weights here, but very different individual-leaf importance.
  // Tiny radiance/camera changes must not switch the whole receiver's budget.
  auto capture = [&](const std::string &name) {
    auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, n;
    auto *data = stbi_load(file.c_str(), &w, &h, &n, 4);
    require(data, "Adaptive sampling capture unavailable");
    std::array<double, 2> mean{};
    for (int y = h / 2 - 32; y < h / 2 + 32; ++y)
      for (int x = w / 2 - 32; x < w / 2 + 32; ++x)
        for (unsigned c = 0; c < 2; ++c) {
          float value = data[(y * w + x) * 4 + c] / 255.f;
          mean[c] += std::pow(value, 2.2f);
        }
    stbi_image_free(data);
    for (auto &value : mean)
      value /= 64 * 64;
    return mean;
  };
  for (auto key : {"bounces", "taa", "localDenoiser", "localGuiding",
                   "accumulate"})
    config(key, "0");
  config("denoiser", "off");
  config("adaptiveLocalSampling", "1");
  config("localSamples", "1");
  config("emissiveCandidates", "4");
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      camera.projection[i][j] = 0;
  camera.projection[0][0] = camera.projection[1][1] = 32;
  camera.projection[2][2] = 100.f / 99.95f;
  camera.projection[2][3] = 1;
  camera.projection[3][2] = -5.f / 99.95f;
  camera.view[3][0] = -192;
  camera.view[3][1] = -65;
  camera.view[3][2] = -191;
  auto update = [&](float scale, float perturbation) {
    for (unsigned i = 0; i < lights.size(); ++i) {
      sphere.position = {i < 2 ? 184.f : 200.f,
                         65 + (i < 2 ? 5.f : .1f) * ((i & 1) ? 1.f : -1.f),
                         191};
      float radiance =
          1200 * scale * (i < 2 ? .1926836f * (1 + perturbation) : 1.f);
      light.radiance = {radiance, radiance, radiance};
      ok(api.UpdateLightDefinition(lights[i], &light));
    }
  };
  auto emissive = material({0, 0, 0}, 1, .01f);
  auto farEmitter = quad(2, 191.f, 240.f, 241.f, 65.f, 66.f, emissive);
  auto *csv = std::fopen((directory / "metrics.csv").c_str(), "w");
  require(csv, "Cannot open adaptive sampling metrics");
  std::fputs("primary,pose,sample_fraction,candidate_fraction\n", csv);
  for (const char *primary : {"raster", "ray"}) {
    config("primaryVisibility", primary);
    meshes = {receiver};
    config("localSamples", "1");
    double minimum = 1, maximum = 0;
    for (int pose = 0; pose < 16; ++pose) {
      float phase = float(pose) * 6.283185307f / 16;
      update(1, .02f * std::sin(phase));
      camera.view[3][0] = -192 + .004f * std::cos(phase);
      auto mean =
          capture(std::string(primary) + "-boundary-" + std::to_string(pose));
      minimum = std::min(minimum, mean[0]);
      maximum = std::max(maximum, mean[0]);
      std::fprintf(csv, "%s,%d,%.9f,%.9f\n", primary, pose, mean[0], mean[1]);
      std::fflush(csv);
    }
    std::printf("%s budget variation %.6f\n", primary, maximum - minimum);
    require(maximum - minimum < .06,
            "Small illumination/motion changes switched a whole surface's "
            "sampling budget");
    update(.001f, 0);
    auto dim = capture(std::string(primary) + "-dim");
    require(std::abs(dim[0] - .25) < .01,
            "Dim receivers lost the one-sample adaptive budget");
    update(10, 0);
    auto bright = capture(std::string(primary) + "-bright");
    require(bright[0] > .99, "Bright receivers did not receive four samples");
    // An emissive source outside the influence range enables the area-light
    // estimator without changing this dim receiver's importance.
    meshes.push_back(farEmitter);
    update(.001f, 0);
    config("localSamples", "3");
    auto floor = capture(std::string(primary) + "-floors");
    require(std::abs(floor[0] - .75) < .01 && floor[1] > .99,
            "Adaptive sampling reduced configured sample/candidate floors");
    config("adaptiveLocalSampling", "0");
    config("localSamples", "1");
    auto disabled = capture(std::string(primary) + "-disabled");
    require(std::abs(disabled[0] - .25) < .01 && disabled[1] > .99,
            "Disabled adaptation did not retain the uniform budget");
    config("adaptiveLocalSampling", "1");
  }
  std::fclose(csv);
  ok(api.Shutdown());
}
