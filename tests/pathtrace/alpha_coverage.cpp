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
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void ok(remixapi_ErrorCode result) { require(!result, "Alpha coverage API failure"); }
} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "Supply screenshot directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  const bool paint = std::getenv("REMIX_TEST_MATERIAL_DECAL") != nullptr;
  const bool benchmark = std::getenv("REMIX_TEST_ALPHA_BENCHMARK") != nullptr;
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#ifndef _WIN32
  startup.windowWidth = benchmark ? 960 : 256;
  startup.windowHeight = benchmark ? 540 : 128;
#endif
  ok(api.Startup(&startup));
  // These fixtures measure the unexposed lighting/display contract.
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
  const char *mode = std::getenv("REMIX_TEST_ALPHA_RECONSTRUCTION");
  if (mode) {
    remixapi_ReconstructionDiagnosticsEXT d{};
    d.structSize = sizeof(d);
    ok(remixapi_GetReconstructionDiagnosticsEXT(&d));
    if (std::string(mode) == "dlss" && !(d.capabilities & 4)) {
      ok(api.Shutdown());
      return 77;
    }
    config("upscaler", mode);
    config("dlssRayReconstruction", "1");
    config("fsrQuality", "quality");
    config("dlssQuality", "quality");
  }
  remixapi_CameraInfoParameterizedEXT parameters{};
  parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  parameters.right = {1, 0, 0};
  parameters.up = {0, 1, 0};
  parameters.forward = {0, 0, 1};
  parameters.aspect = benchmark ? 960.f / 540.f : 2;
  parameters.fovYInDegrees = 90;
  parameters.nearPlane = .1f;
  parameters.farPlane = 100;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &parameters;
  // Black and colored fully transparent texels must give the same background.
  uint8_t pixels[] = {0, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0, 128, 255, 0, 0, 255};
  remixapi_TextureInfo texture{};
  texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  texture.hash = 1;
  texture.width = 4;
  texture.height = 1;
  texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  texture.data = pixels;
  texture.dataSize = sizeof(pixels);
  remixapi_TextureHandle textureHandle{};
  ok(api.CreateTexture(&texture, &textureHandle));
#ifdef _WIN32
  const auto texturePath = L"0x1";
#else
  const auto texturePath = "0x1";
#endif
  auto material = [&](uint64_t hash, bool foreground, bool emissive, float opacity = 1) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = opacity;
    opaque.alphaTestType = 7;
    opaque.useDrawCallAlphaState = 1;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = hash;
    info.albedoTexture = foreground ? texturePath : nullptr;
    info.emissiveTexture = foreground && emissive ? texturePath : nullptr;
    info.emissiveIntensity = emissive ? 1 : 0;
    info.emissiveColorConstant = {1, 1, 1};
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  auto quad = [&](uint64_t hash, remixapi_MaterialHandle material, float z) {
    remixapi_HardcodedVertex vertices[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      vertices[i].position[0] = (i == 0 || i == 3 ? -2.f : 2.f) * z;
      vertices[i].position[1] = (i < 2 ? -1.f : 1.f) * z;
      vertices[i].position[2] = z;
      vertices[i].color = 0xffffffff;
      vertices[i].texcoord[0] = i == 0 || i == 3 ? 0 : 1;
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
  auto background = quad(4, material(2, false, true), 3);
  auto foreground = quad(5, material(3, true, true), 2);
  remixapi_InstanceInfoBlendEXT blend{};
  blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  blend.alphaBlendEnabled = 1;
  blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
  blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, 0, 1};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 6;
  remixapi_LightHandle sunlight{};
  ok(api.CreateLight(&light, &sunlight));
  unsigned layers = 1;
  bool feedback = false;
  float worldOffset = 0, foregroundOffset = 0;
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sunlight));
    for (auto mesh : {background, foreground})
      for (unsigned layer = 0; layer < (mesh == background ? 1u : layers);
           ++layer) {
        remixapi_InstanceIdentityEXT identity{};
        identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
        identity.instanceId = mesh == background ? 1 : 2 + layer;
        identity.rasterVisible = 1;
        identity.pNext = mesh == foreground ? &blend : nullptr;
        remixapi_InstanceInfo instance{};
        instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        instance.pNext = &identity;
        instance.mesh = mesh;
        if (paint && mesh == foreground)
          instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
        instance.doubleSided = 1;
        for (int i = 0; i < 3; ++i)
          instance.transform.matrix[i][i] = 1;
        instance.transform.matrix[2][3] =
            worldOffset +
            (mesh == foreground ? foregroundOffset - .02f * layer : 0);
        ok(api.DrawInstance(&instance));
      }
    if (feedback) {
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 100;
      identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
      identity.rasterVisible = 1;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &identity;
      instance.mesh = background;
      instance.doubleSided = 1;
      for (int i = 0; i < 3; ++i)
        instance.transform.matrix[i][i] = 1;
      instance.transform.matrix[2][3] = -.5f;
      ok(api.DrawInstance(&instance));
    }
    ok(api.Present(nullptr));
  });
  if (benchmark) {
    config("bounces", "2");
    config("localRange", "0");
    material(2, false, false);
    light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
    ok(api.UpdateLightDefinition(sunlight, &light));
    std::puts("primary,alpha,layers,camera_z,trace_median_ms,trace_p95_ms");
    for (const char *primary : {"raster", "ray"}) {
      config("primaryVisibility", primary);
      for (int alpha : {0, 128, 255}) {
        for (unsigned i = 0; i < 4; ++i) pixels[4 * i + 3] = uint8_t(alpha);
        ok(api.CreateTexture(&texture, &textureHandle));
        material(3, true, false);
        for (unsigned count : {1u, 8u}) for (float position : {-4.f, 1.6f}) {
          layers = count;
          parameters.position.z = position;
          for (unsigned i = 0; i < 12; ++i) frame();
          std::vector<float> times;
          for (unsigned i = 0; i < 40; ++i) {
            frame();
            remixapi_TracingStatisticsEXT stats{}; stats.structSize = sizeof(stats);
            ok(remixapi_GetTracingStatisticsEXT(&stats));
            require(stats.gpuTimingsAvailable, "Alpha benchmark requires GPU timestamps");
            times.push_back(stats.gpuTraceMilliseconds);
          }
          std::sort(times.begin(), times.end());
          std::printf("%s,%d,%u,%g,%.6f,%.6f\n", primary, alpha, layers, position,
                      times[times.size()/2], times[size_t(times.size()*.95)]);
          std::fflush(stdout);
        }
      }
    }
    ok(api.Shutdown());
    return 0;
  }
  auto capture = [&](const std::string &label) {
    // Material paint retains receiver depth/motion across edits. Allow the
    // upscaler's color history to settle before checking static coverage math.
    for (unsigned i = 0; i < (mode ? (paint ? 96u : 32u) : 3u); ++i)
      frame();
    const auto file = (directory / (label + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, channels;
    auto *data = stbi_load(file.c_str(), &w, &h, &channels, 4);
    require(data && w == 256 && h == 128, "Alpha capture unavailable");
    std::array<std::array<float, 3>, 4> result{};
    for (unsigned sample = 0; sample < 4; ++sample)
      for (int y = h / 2 - 4; y < h / 2 + 4; ++y)
        for (int x = int(sample * 64 + 28); x < int(sample * 64 + 36); ++x)
          for (int c = 0; c < 3; ++c)
            result[sample][c] += data[(y * w + x) * 4 + c] / (255.f * 64);
    stbi_image_free(data);
    if (mode) {
      remixapi_ReconstructionDiagnosticsEXT d{};
      d.structSize = sizeof(d);
      ok(remixapi_GetReconstructionDiagnosticsEXT(&d));
      require(d.activeBackend == (std::string(mode) == "dlss" ? 3u : 1u),
              "Alpha test reconstruction fell back");
    }
    std::printf("%s: black hole %.3f %.3f %.3f, colored hole %.3f %.3f %.3f, half %.3f %.3f %.3f, opaque %.3f %.3f %.3f\n",
                label.c_str(), result[0][0], result[0][1], result[0][2], result[1][0], result[1][1],
                result[1][2], result[2][0], result[2][1], result[2][2], result[3][0], result[3][1], result[3][2]);
    return result;
  };
  for (const char *primary : {"ray", "raster"}) {
    pixels[3] = pixels[7] = 0; pixels[11] = 128; pixels[15] = 255;
    ok(api.CreateTexture(&texture, &textureHandle));
    layers = 1;
    config("primaryVisibility", primary);
    material(2, false, true);
    material(3, true, true);
    light.radiance = {};
    ok(api.UpdateLightDefinition(sunlight, &light));
    auto samples = capture(std::string(primary) + "-coverage");
    for (unsigned hole : {0u, 1u})
      for (float c : samples[hole])
        require(c > .95f, "Transparent decal RGB darkened the background");
    require(samples[2][0] > .95f && std::abs(samples[2][1] - .734f) < .08f &&
                std::abs(samples[2][2] - .734f) < .08f,
            "Partial alpha does not follow source-over coverage");
    require(samples[3][0] > .95f && samples[3][1] < .04f && samples[3][2] < .04f,
            "Opaque decal lost foreground color");
    if (paint && mode) {
      for (unsigned i = 0; i < 160; ++i) frame();
      remixapi_TracingStatisticsEXT stats{};
      stats.structSize = sizeof(stats);
      ok(remixapi_GetTracingStatisticsEXT(&stats));
      require(!stats.redrawRequested, "Settled reconstruction kept requesting static redraws");
    }
    if (paint) {
      const auto originalPixels = std::to_array(pixels);
      for (unsigned i = 0; i < 4; ++i) {
        pixels[4*i] = 255; pixels[4*i+1] = pixels[4*i+2] = 0; pixels[4*i+3] = 128;
      }
      ok(api.CreateTexture(&texture, &textureHandle));
      material(3, true, true);
      worldOffset = parameters.position.z = 10000;
      foregroundOffset = .99f; // .01 in front of the receiver; smaller than a normal ray offset.
      samples = capture(std::string(primary) + "-close-paint-large-world");
      for (unsigned probe : {1u, 2u})
        require(samples[probe][0] > .95f && std::abs(samples[probe][1] - .734f) < .08f &&
                    std::abs(samples[probe][2] - .734f) < .08f,
                "Decal resolve skipped a close receiver at large world coordinates");
      worldOffset = parameters.position.z = foregroundOffset = 0;
      std::copy(originalPixels.begin(), originalPixels.end(), pixels);
      ok(api.CreateTexture(&texture, &textureHandle));
      material(3, true, true);
    }
    blend.textureAlphaOperation = 3;
    blend.textureAlphaArg1Source = 1;
    blend.textureAlphaArg2Source = 3;
    blend.tFactor = 0x80ffffff;
    samples = capture(std::string(primary) + "-factor-alpha");
    require(samples[3][0] > .95f && std::abs(samples[3][1] - .734f) < .08f &&
                std::abs(samples[3][2] - .734f) < .08f,
            "Texture-factor opacity was promoted to opaque");
    blend.textureAlphaOperation = 0;
    blend.textureAlphaArg1Source = blend.textureAlphaArg2Source = 0;
    blend.tFactor = 0;
    blend.alphaTestEnabled = 1;
    blend.alphaTestCompareOp = 4;
    blend.alphaTestReferenceValue = 192;
    samples = capture(std::string(primary) + "-cutout");
    for (unsigned hole : {0u, 1u, 2u}) for (float c : samples[hole])
      require(c > .95f, "Blended cutout did not preserve its alpha comparison");
    require(samples[3][0] > .95f && samples[3][1] < .04f && samples[3][2] < .04f,
            "Cutout removed the opaque texel");
    blend.alphaTestEnabled = 0;
    feedback = true;
    samples = capture(std::string(primary) + "-feedback");
    for (const auto &sample : samples) for (float c : sample)
      require(c > .95f, "Blended draw incorrectly occluded raster feedback");
    feedback = false;
    material(2, false, false);
    material(3, true, false);
    light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
    ok(api.UpdateLightDefinition(sunlight, &light));
    samples = capture(std::string(primary) + "-shadow");
    for (unsigned hole : {0u, 1u})
      for (float c : samples[hole])
        require(c > .95f, "Transparent decal RGB blocked shadow rays");
    light.radiance = {};
    ok(api.UpdateLightDefinition(sunlight, &light));
    material(2, false, true);
    material(3, true, false, 0.f);
    samples = capture(std::string(primary) + "-zero-opacity-tint");
    if (paint) {
      for (const auto &sample : samples) for (float c : sample)
        require(c > .95f, "Zero-opacity paint tinted its receiver");
    } else {
    require(samples[0][0] < .04f && samples[0][1] < .04f && samples[0][2] < .04f,
            "Zero-opacity black tinted sheet was incorrectly skipped");
    require(samples[1][0] < .04f && samples[1][1] > .95f && samples[1][2] < .04f,
            "Zero-opacity colored sheet lost its transmission tint");
    }
    for (unsigned i = 0; i < 4; ++i) pixels[4 * i + 3] = 0;
    ok(api.CreateTexture(&texture, &textureHandle));
    material(3, true, false);
    layers = 40;
    samples = capture(std::string(primary) + "-empty-stack");
    for (const auto &sample : samples) for (float c : sample)
      require(c > .95f, "Empty alpha layers consumed the transparent traversal budget");
  }
  ok(api.Shutdown());
  std::puts("Alpha coverage preserves black/colored holes, partial opacity and shadow transmission");
}
