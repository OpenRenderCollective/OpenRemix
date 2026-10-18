// Metal backend rendering: the sample cubes (double-sided) on a ground plane,
// rasterized and path traced through the public API. The presented
// screenshots are checked at a few known points: lit ground, the cube's
// shadow, its sunlit green face and the sky.
#include "remix-native/scene.h"
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <remix/remix_reconstruction_ext.h>
#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
  remixapi_Interface api = {};
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void checked(remixapi_ErrorCode code, int line) {
    if (code) {
      std::fprintf(stderr, "Unexpected error: 0x%x at line %d\n", unsigned(code), line);
      std::exit(1);
    }
  }
#define ok(code) checked((code), __LINE__)

  struct Image {
    int width = 0, height = 0;
    unsigned char *pixels = nullptr;
    ~Image() { stbi_image_free(pixels); }
    // Linear-ish brightness and color of a small area at normalized coordinates.
    void sample(float x, float y, float rgb[3]) const {
      const int cx = int(x * width), cy = int(y * height), r = std::max(2, width / 200);
      double sum[3] = {};
      int count = 0;
      for (int py = cy - r; py <= cy + r; ++py)
        for (int px = cx - r; px <= cx + r; ++px, ++count)
          for (int c = 0; c < 3; ++c)
            sum[c] += pixels[(size_t(py) * width + px) * 4 + c] / 255.0;
      for (int c = 0; c < 3; ++c)
        rgb[c] = float(sum[c] / count);
    }
    float luminance(float x, float y) const {
      float rgb[3];
      sample(x, y, rgb);
      return .2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2];
    }
  };

  struct GroundScene {
    SampleScene sample;
    remixapi_MeshHandle plane = nullptr;
    void create() {
      ok(sample.create(api));
      remixapi_MaterialInfoOpaqueEXT opaque = {};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.albedoConstant = {.8f, .8f, .8f};
      opaque.opacityConstant = 1;
      opaque.alphaTestType = 7;
      remixapi_MaterialInfo material = {};
      material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      material.pNext = &opaque;
      material.hash = 0x201;
      remixapi_MaterialHandle handle = nullptr;
      ok(api.CreateMaterial(&material, &handle));
      remixapi_HardcodedVertex vertices[4] = {};
      const float corners[4][2] = {{-20, -20}, {20, -20}, {20, 20}, {-20, 20}};
      for (int i = 0; i < 4; ++i) {
        vertices[i].position[0] = corners[i][0];
        vertices[i].position[1] = -1;
        vertices[i].position[2] = corners[i][1] + 8;
        vertices[i].normal[1] = 1;
        vertices[i].color = 0xffffffff;
      }
      const uint32_t indices[6] = {0, 2, 1, 0, 3, 2};
      remixapi_MeshInfoSurfaceTriangles surface = {};
      surface.vertices_values = vertices;
      surface.vertices_count = 4;
      surface.indices_values = indices;
      surface.indices_count = 6;
      surface.material = handle;
      remixapi_MeshInfo mesh = {};
      mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      mesh.hash = 0x202;
      mesh.surfaces_count = 1;
      mesh.surfaces_values = &surface;
      ok(api.CreateMeshBatched(&mesh, &plane));
    }
    void draw(uint32_t width, uint32_t height) {
      ok(api.DrawLightInstance(sample.daylight));
      remixapi_CameraInfoParameterizedEXT parameters = {};
      parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
      parameters.position = {0, 1.5f, -1};
      parameters.forward = {0, -.2f, 1};
      parameters.up = {0, 1, .2f};
      parameters.right = {1, 0, 0};
      parameters.aspect = height ? float(width) / height : 1;
      parameters.fovYInDegrees = 60;
      parameters.nearPlane = .05f;
      parameters.farPlane = 100;
      remixapi_CameraInfo camera = {};
      camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
      camera.pNext = &parameters;
      ok(api.SetupCamera(&camera));
      const float t = .6f;
      remixapi_InstanceInfo instance = {};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.mesh = sample.mesh;
      instance.doubleSided = 1;
      instance.transform.matrix[0][0] = std::cos(t);
      instance.transform.matrix[0][2] = std::sin(t);
      instance.transform.matrix[1][1] = 1;
      instance.transform.matrix[2][0] = -std::sin(t);
      instance.transform.matrix[2][2] = std::cos(t);
      instance.transform.matrix[2][3] = 6;
      ok(api.DrawInstance(&instance));
      instance.transform.matrix[0][3] = 2.5f;
      instance.transform.matrix[2][3] = 9;
      ok(api.DrawInstance(&instance));
      remixapi_InstanceInfo floor = {};
      floor.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      floor.mesh = plane;
      floor.doubleSided = 1;
      floor.transform.matrix[0][0] = floor.transform.matrix[1][1] = floor.transform.matrix[2][2] = 1;
      ok(api.DrawInstance(&floor));
    }
  };

  // Presents frames and returns the screenshot of the last one.
  void render(GroundScene &scene, unsigned frames, const std::string &path, Image &image) {
    std::filesystem::remove(path);
    for (unsigned frame = 0; frame < frames; ++frame) {
      ok(api.PumpEvents());
      remixapi_WindowState state = {};
      ok(api.GetWindowState(&state));
      scene.draw(state.drawableWidth, state.drawableHeight);
      if (frame + 1 == frames)
        ok(api.RequestPresentedScreenshot(path.c_str()));
      ok(api.Present(nullptr));
    }
    // The capture is written when the next presentation begins.
    ok(api.PumpEvents());
    scene.draw(1, 1);
    ok(api.Present(nullptr));
    int channels = 0;
    image.pixels = stbi_load(path.c_str(), &image.width, &image.height, &channels, 4);
    require(image.pixels != nullptr, "Presented screenshot was not written");
  }
} // namespace

int main(int argc, char **argv) {
  require(argc == 3, "usage: remix-metal-render-test LIBRARY OUTPUT_DIRECTORY");
  const std::string directory = argv[2];
  std::filesystem::create_directories(directory);
  remixapi_HMODULE library = nullptr;
  ok(remixapi_lib_loadRemixDllAndInitialize(argv[1], &api, &library));
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.windowTitle = "Remix Metal render test";
  ok(api.Startup(&startup));
  GroundScene scene;
  scene.create();
  // The default profile may path trace (runtime/settings.cpp): rasterize first.
  ok(api.SetConfigVariable("rtx.native.backend", "raster"));

  Image raster;
  render(scene, 3, directory + "/raster.png", raster);
  float rgb[3];
  raster.sample(.14f, .84f, rgb);
  require(rgb[0] > .1f && std::abs(rgb[0] - rgb[2]) < .05f, "Raster: the ground plane is missing");
  raster.sample(.557f, .509f, rgb);
  require(rgb[1] > rgb[0] + .1f, "Raster: the cube's green face is missing");

  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "1"));
  // The defaults are a tuned game profile's: the references compare the
  // passes under the settings these checks were written for.
  for (const auto &[key, value] : {std::pair{"bounces", "2"}, {"russianRoulette", "0"}, {"sharcSceneScale", "50"},
                                   {"separateLocalLighting", "0"}, {"localFireflyFilter", "0"}, {"pbrLocalReuse", "0"},
                                   {"translucencyReuse", "0"}, {"viewModel", "0"}, {"upscaler", "auto"}})
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  const auto capabilitiesOf = reinterpret_cast<PFN_remixapi_GetBackendCapabilitiesEXT>(
      dlsym(library, "remixapi_GetBackendCapabilitiesEXT"));
  const auto tracingOf = reinterpret_cast<PFN_remixapi_GetTracingStatisticsEXT>(
      dlsym(library, "remixapi_GetTracingStatisticsEXT"));
  require(capabilitiesOf && tracingOf, "Backend and tracing statistics are not exported");
  // The path tracer is prepared on a thread (pipelines compile on first use):
  // frames present meanwhile. Wait until it traces with accumulation.
  remixapi_BackendCapabilitiesEXT capabilities = {.structSize = sizeof(capabilities)};
  remixapi_TracingStatisticsEXT tracing = {.structSize = sizeof(tracing)};
  for (unsigned frame = 0; frame < 200000; ++frame) {
    ok(api.PumpEvents());
    remixapi_WindowState state = {};
    ok(api.GetWindowState(&state));
    scene.draw(state.drawableWidth, state.drawableHeight);
    ok(api.Present(nullptr));
    ok(capabilitiesOf(&capabilities));
    ok(tracingOf(&tracing));
    if (capabilities.activeBackend == 1 && tracing.accumulationEnabled)
      break;
  }
  require(capabilities.activeBackend == 1, "Path tracing did not activate");
  Image traced;
  render(scene, 64, directory + "/pathtrace.png", traced);
  const float lit = traced.luminance(.14f, .84f), shadow = traced.luminance(.386f, .674f);
  std::printf("lit ground %.3f, shadow %.3f\n", lit, shadow);
  require(lit > .3f, "Path tracing: the sunlit ground is dark");
  require(shadow < lit * .6f, "Path tracing: the cube casts no shadow");
  require(shadow > .005f, "Path tracing: the shadow has no indirect light");
  traced.sample(.557f, .509f, rgb);
  require(rgb[1] > rgb[0] + .1f && rgb[1] > rgb[2], "Path tracing: the cube's sunlit green face is missing");
  traced.sample(.5f, .12f, rgb);
  require(rgb[2] > rgb[0] && rgb[2] < .4f, "Path tracing: the sky is wrong");

  // The temporal stack (NRD REBLUR/SIGMA, TAA, SHARC by default) replaces
  // accumulation: a new configuration is prepared while frames present.
  // Reconstruction is off here (MetalFX is checked below).
  ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "off"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "0"));
  const auto denoisingOf = reinterpret_cast<PFN_remixapi_GetDenoisingStatisticsEXT>(
      dlsym(library, "remixapi_GetDenoisingStatisticsEXT"));
  require(denoisingOf != nullptr, "remixapi_GetDenoisingStatisticsEXT is not exported");
  remixapi_DenoisingStatisticsEXT denoising = {.structSize = sizeof(denoising)};
  for (unsigned frame = 0; frame < 200000; ++frame) {
    ok(api.PumpEvents());
    remixapi_WindowState state = {};
    ok(api.GetWindowState(&state));
    scene.draw(state.drawableWidth, state.drawableHeight);
    ok(api.Present(nullptr));
    ok(denoisingOf(&denoising));
    if (denoising.activeDenoiser && denoising.activeTaa && denoising.historyFrames >= 32)
      break;
  }
  require(denoising.activeDenoiser && denoising.activeTaa, "The denoiser and TAA did not activate");
  Image denoised;
  render(scene, 32, directory + "/denoised.png", denoised);
  const float denoisedLit = denoised.luminance(.14f, .84f), denoisedShadow = denoised.luminance(.386f, .674f);
  std::printf("denoised lit ground %.3f, shadow %.3f\n", denoisedLit, denoisedShadow);
  // Single-sample frames are denoised to the accumulated reference's levels.
  require(std::abs(denoisedLit - lit) < .05f, "Denoised ground differs from the accumulated reference");
  require(std::abs(denoisedShadow - shadow) < .05f, "Denoised shadow differs from the accumulated reference");
  denoised.sample(.557f, .509f, rgb);
  require(rgb[1] > rgb[0] + .1f && rgb[1] > rgb[2], "Denoising: the cube's sunlit green face is missing");

  // MetalFX reconstruction: the temporal scaler upscales a performance-preset
  // render (with NRD), and on macOS 26 the denoised scaler replaces NRD and
  // TAA. Both converge to the accumulated reference's levels.
  const auto reconstructionOf = reinterpret_cast<decltype(&remixapi_GetReconstructionDiagnosticsEXT)>(
      dlsym(library, "remixapi_GetReconstructionDiagnosticsEXT"));
  require(reconstructionOf != nullptr, "remixapi_GetReconstructionDiagnosticsEXT is not exported");
  remixapi_ReconstructionDiagnosticsEXT reconstruction = {.structSize = sizeof(reconstruction)};
  const auto reconstruct = [&](uint32_t backend, const char *name) {
    for (unsigned frame = 0; frame < 200000; ++frame) {
      ok(api.PumpEvents());
      remixapi_WindowState state = {};
      ok(api.GetWindowState(&state));
      scene.draw(state.drawableWidth, state.drawableHeight);
      ok(api.Present(nullptr));
      ok(reconstructionOf(&reconstruction));
      if (reconstruction.activeBackend == backend && reconstruction.evaluatedFrames >= 32)
        break;
    }
    if (reconstruction.activeBackend != backend) {
      std::fprintf(stderr, "%s did not activate: %s\n", name, reconstruction.fallbackReason);
      std::exit(1);
    }
    require(reconstruction.renderWidth < reconstruction.displayWidth, "MetalFX did not render below display size");
    Image image;
    render(scene, 32, directory + "/" + name + ".png", image);
    const float imageLit = image.luminance(.14f, .84f), imageShadow = image.luminance(.386f, .674f);
    std::printf("%s (%ux%u -> %ux%u): lit ground %.3f, shadow %.3f\n", name, reconstruction.renderWidth,
                reconstruction.renderHeight, reconstruction.displayWidth, reconstruction.displayHeight, imageLit,
                imageShadow);
    if (std::abs(imageLit - lit) >= .05f || std::abs(imageShadow - shadow) >= .05f) {
      std::fprintf(stderr, "%s differs from the accumulated reference\n", name);
      std::exit(1);
    }
    image.sample(.557f, .509f, rgb);
    if (!(rgb[1] > rgb[0] + .1f && rgb[1] > rgb[2])) {
      std::fprintf(stderr, "%s: the cube's sunlit green face is missing\n", name);
      std::exit(1);
    }
  };
  ok(reconstructionOf(&reconstruction));
  if (reconstruction.capabilities & 2) {
    ok(api.SetConfigVariable("rtx.native.pathtrace.dlssRayReconstruction", "0"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.dlssQuality", "performance"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "auto"));
    reconstruct(1, "metalfx-upscaling");
    // Frame generation presents interpolated frames between rendered ones;
    // the screenshot is the rendered frame.
    ok(api.SetConfigVariable("rtx.native.frameGeneration", "1"));
    reconstruct(1, "metalfx-frame-generation");
    ok(api.SetConfigVariable("rtx.native.frameGeneration", "0"));
  } else
    std::printf("MetalFX upscaling unavailable: %s\n", reconstruction.fallbackReason);
  if (reconstruction.capabilities & 4) {
    ok(api.SetConfigVariable("rtx.native.pathtrace.dlssRayReconstruction", "1"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.dlssQuality", "quality"));
    reconstruct(3, "metalfx-denoising");
  } else
    std::printf("MetalFX denoising unavailable (macOS 26)\n");

  ok(remixapi_lib_shutdownAndUnloadRemixDll(&api, library));
  return 0;
}
