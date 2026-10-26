// rtx.native.pathtrace.shaderExecutionReordering: the opaque lighting pass as
// a ray-generation shader that reorders invocations by hit surface. A
// divergent scene (textured, rough and metallic boxes, two bounces, ray-traced
// primary visibility) is captured with SER off, on and off again. The off
// captures establish a deterministic fixture; the SER capture must match it
// (the raygen pipeline is compiled separately, so its floating-point rounding
// may differ in the last bits), and GPU pass timings must show SER ran.
#include "api/exports.h"
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  void ok(remixapi_ErrorCode result) { require(!result, "Shader execution reordering API failure"); }
  // REMIX_TEST_SER_BENCHMARK: a 1080p, four-bounce, 120-box variant whose
  // printed opaque-pass times compare compute and reordered tracing.
  const bool benchmark = std::getenv("REMIX_TEST_SER_BENCHMARK") != nullptr;
  const uint32_t width = benchmark ? 1920 : 256, height = benchmark ? 1080 : 160;

  // The opaque pass of the latest measured frame: GPU milliseconds and whether
  // it ran as ray generation with reordering.
  struct OpaquePass {
    bool found = false, reordered = false;
    double milliseconds = 0;
  };
  OpaquePass opaquePass() {
    remixapi_GpuPassTimingsEXT timings{};
    timings.structSize = sizeof(timings);
    OpaquePass result;
    if (remixapi_GetGpuPassTimingsEXT(&timings) != REMIXAPI_ERROR_CODE_SUCCESS || !timings.available)
      return result;
    for (uint32_t i = 0; i < timings.passCount; ++i)
      if (std::strncmp(timings.passes[i].name, "Trace opaque", 12) == 0) {
        result.found = true;
        result.reordered = std::strstr(timings.passes[i].name, "(SER)") != nullptr;
        result.milliseconds = timings.passes[i].milliseconds;
      }
    return result;
  }
} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "Supply screenshot directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  // One sample sequence for every frame (C runtime environment: SDL's setenv
  // does not update it on Windows).
#if defined(_WIN32)
  _putenv_s("REMIX_PATH_FIXED_SEED", "7");
#else
  setenv("REMIX_PATH_FIXED_SEED", "7", 1);
#endif
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = width;
  startup.windowHeight = height;
#endif
  ok(api.Startup(&startup));
  SDL_SetWindowSize(remix::api::g_runtime.window().handle(), int(width), int(height));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2) || !remix::api::g_runtime.invocationReorderSupported()) {
    std::puts("SKIP: no ray tracing invocation reorder support");
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  ok(api.SetConfigVariable("rtx.native.gpuPassTimers", "1"));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  for (const char *key : {"sharc", "sigma", "taa", "autoExposure", "bloom", "accumulate"})
    config(key, "0");
  config("primaryVisibility", "ray");
  config("denoiser", "off");
  config("upscaler", "off");
  config("samples", "1");
  config("bounces", benchmark ? "4" : "2");

  remixapi_CameraInfoParameterizedEXT parameters{};
  parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  parameters.right = {1, 0, 0};
  parameters.up = {0, 1, 0};
  parameters.forward = {0, 0, 1};
  parameters.aspect = float(width) / height;
  parameters.fovYInDegrees = 75;
  parameters.nearPlane = .1f;
  parameters.farPlane = 200;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &parameters;

  uint32_t seed = 11;
  auto random = [&] {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) / float(1u << 24);
  };
  // Eight materials, each with its own texture and PBR response.
  std::vector<remixapi_MaterialHandle> materials;
  for (uint64_t m = 0; m < 8; ++m) {
    std::vector<uint8_t> pixels(16 * 16 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
      const float shade = .5f + .5f * random();
      pixels[i] = uint8_t(255 * shade * (m & 1 ? 1.f : .3f));
      pixels[i + 1] = uint8_t(255 * shade * (m & 2 ? 1.f : .3f));
      pixels[i + 2] = uint8_t(255 * shade * (m & 4 ? 1.f : .3f));
      pixels[i + 3] = 255;
    }
    remixapi_TextureInfo texture{};
    texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    texture.hash = 100 + m;
    texture.width = texture.height = 16;
    texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    texture.data = pixels.data();
    texture.dataSize = pixels.size();
    remixapi_TextureHandle handle{};
    ok(api.CreateTexture(&texture, &handle));
#ifdef _WIN32
    wchar_t name[32];
    std::swprintf(name, 32, L"0x%llx", static_cast<unsigned long long>(100 + m));
#else
    char name[32];
    std::snprintf(name, sizeof(name), "0x%llx", static_cast<unsigned long long>(100 + m));
#endif
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = .1f + .12f * m;
    opaque.metallicConstant = m % 3 == 0 ? 1.f : 0.f;
    opaque.alphaTestType = 7;
    // REMIX_TEST_SER_MATERIALS: pbr (default), legacy, or mixed (alternating).
    const char *classes = std::getenv("REMIX_TEST_SER_MATERIALS");
    const bool legacy = classes && (std::strcmp(classes, "legacy") == 0 || (std::strcmp(classes, "mixed") == 0 && m % 2));
    remixapi_MaterialInfoPbrEXT pbr{};
    pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    pbr.enabled = legacy ? 0 : 1;
    opaque.pNext = &pbr;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = 200 + m;
    info.albedoTexture = name;
    remixapi_MaterialHandle material{};
    ok(api.CreateMaterial(&info, &material));
    materials.push_back(material);
  }
  // An axis-aligned box of six outward-facing quads.
  auto box = [&](uint64_t hash, remixapi_MaterialHandle material, const float low[3], const float high[3]) {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    for (int axis = 0; axis < 3; ++axis)
      for (int side = 0; side < 2; ++side) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const uint32_t base = uint32_t(vertices.size());
        for (int corner = 0; corner < 4; ++corner) {
          remixapi_HardcodedVertex vertex{};
          vertex.position[axis] = side ? high[axis] : low[axis];
          vertex.position[u] = corner == 1 || corner == 2 ? high[u] : low[u];
          vertex.position[v] = corner >= 2 ? high[v] : low[v];
          vertex.normal[axis] = side ? 1.f : -1.f;
          vertex.texcoord[0] = corner == 1 || corner == 2 ? 1.f : 0.f;
          vertex.texcoord[1] = corner >= 2 ? 1.f : 0.f;
          vertex.color = 0xffffffff;
          vertices.push_back(vertex);
        }
        const bool flip = side == 0;
        for (uint32_t k : {0u, flip ? 2u : 1u, flip ? 1u : 2u, 0u, flip ? 3u : 2u, flip ? 2u : 3u})
          indices.push_back(base + k);
      }
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices.data();
    surface.vertices_count = vertices.size();
    surface.indices_values = indices.data();
    surface.indices_count = indices.size();
    surface.material = material;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle mesh{};
    ok(api.CreateMesh(&info, &mesh));
    return mesh;
  };
  std::vector<remixapi_MeshHandle> meshes;
  const float floorLow[3] = {-12, -2.2f, 1}, floorHigh[3] = {12, -2, 30};
  meshes.push_back(box(300, materials[1], floorLow, floorHigh));
  const float wallLow[3] = {-12, -2, 22}, wallHigh[3] = {12, 8, 22.5f};
  meshes.push_back(box(301, materials[6], wallLow, wallHigh));
  for (uint64_t i = 0; i < (benchmark ? 120u : 30u); ++i) {
    const float x = -8 + 16 * random(), z = 4 + 16 * random(), size = .4f + 1.2f * random();
    const float y = -2 + size * random() * 2;
    const float low[3] = {x - size, -2, z - size}, high[3] = {x + size, y + size, z + size};
    meshes.push_back(box(310 + i, materials[i % materials.size()], low, high));
  }
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  const float length = std::sqrt(.4f * .4f + .8f * .8f + .45f * .45f);
  distant.direction = {.4f / length, -.8f / length, .45f / length};
  distant.angularDiameterDegrees = .5f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 20;
  light.radiance = {4, 4, 4};
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&light, &sun));

  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    uint64_t id = 1;
    for (auto mesh : meshes) {
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = id++;
      identity.rasterVisible = 1;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &identity;
      instance.mesh = mesh;
      instance.doubleSided = 1;
      for (int i = 0; i < 3; ++i) instance.transform.matrix[i][i] = 1;
      ok(api.DrawInstance(&instance));
    }
    ok(api.Present(nullptr));
  });
  struct Capture {
    std::vector<uint8_t> pixels;
    double milliseconds = 0; // Median opaque pass GPU time.
  };
  // Waits until the opaque pass runs in the requested form (SER pipelines
  // compile in the background), then measures and captures.
  auto capture = [&](const std::string &name, bool reordered) {
    const auto start = std::chrono::steady_clock::now();
    unsigned settled = 0;
    while (settled < 8) {
      frame();
      const auto pass = opaquePass();
      settled = pass.found && pass.reordered == reordered ? settled + 1 : 0;
      require(std::chrono::steady_clock::now() - start < std::chrono::minutes(8),
              reordered ? "The opaque pass never ran with shader execution reordering"
                        : "The opaque pass did not return to compute");
    }
    std::vector<double> times;
    for (unsigned i = 0; i < 31; ++i) {
      frame();
      const auto pass = opaquePass();
      require(pass.found && pass.reordered == reordered, "The opaque pass changed form while measuring");
      times.push_back(pass.milliseconds);
    }
    std::sort(times.begin(), times.end());
    const auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, channels;
    auto *data = stbi_load(file.c_str(), &w, &h, &channels, 4);
    require(data && w == int(width) && h == int(height), "Capture unavailable");
    Capture result{{data, data + size_t(w) * h * 4}, times[times.size() / 2]};
    stbi_image_free(data);
    std::printf("%s: opaque pass %.3f ms (median of %zu)\n", name.c_str(), result.milliseconds, times.size());
    return result;
  };
  const auto off = capture("compute", false);
  config("shaderExecutionReordering", "1");
  const auto on = capture("reordered", true);
  config("shaderExecutionReordering", "0");
  const auto again = capture("compute-again", false);
  auto compare = [](const Capture &a, const Capture &b, size_t &different, int &maximum, double &mean) {
    different = 0;
    maximum = 0;
    mean = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
      const int d = std::abs(int(a.pixels[i]) - int(b.pixels[i]));
      different += d != 0;
      maximum = std::max(maximum, d);
      mean += d;
    }
    mean /= double(a.pixels.size());
  };
  size_t different;
  int maximum;
  double mean;
  compare(off, again, different, maximum, mean);
  std::printf("compute/compute-again: %zu differing bytes\n", different);
  require(different == 0, "The fixture is not deterministic; the comparison would be meaningless");
  compare(off, on, different, maximum, mean);
  std::printf("compute/reordered: %zu differing bytes (%.4f%%), maximum %d/255, mean %.5f/255\n", different,
              100.0 * double(different) / double(off.pixels.size()), maximum, mean);
  require(mean < .05 && different * 100 < off.pixels.size(),
          "Shader execution reordering changed the image beyond floating-point rounding");
  ok(api.Shutdown());
  std::puts("PASS");
  return 0;
}
