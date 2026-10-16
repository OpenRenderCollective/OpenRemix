#include "postfx/meter.h"
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <thread>
#include <source_location>
std::atomic<bool> failPostImage{false}, holdPostImage{false},
    imageEntered{false};
std::mutex imageMutex;
std::condition_variable imageGate;
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice d, const VkImageCreateInfo *i,
              const VkAllocationCallbacks *a, VkImage *out) {
  if (holdPostImage) {
    imageEntered = true;
    std::unique_lock lock(imageMutex);
    imageGate.wait(lock, [] { return !holdPostImage; });
  }
  if (failPostImage.exchange(false)) {
    *out = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  static auto original =
      reinterpret_cast<PFN_vkCreateImage>(dlsym(RTLD_NEXT, "vkCreateImage"));
  return original(d, i, a, out);
}
void require(bool v, const char *m) {
  if (!v) {
    holdPostImage = false;
    imageGate.notify_all();
    std::fprintf(stderr, "%s\n", m);
    std::exit(1);
  }
}
void ok(remixapi_ErrorCode r, std::source_location location = std::source_location::current()) {
  if (r) std::fprintf(stderr, "API error %u at %s:%u\n", r, location.file_name(), location.line());
  require(!r, "Postfx runtime API failure");
}
int main(int argc, char **argv) {
  require(argc > 1, "Missing output directory");
  std::filesystem::create_directories(argv[1]);
  const auto linearPath =
      (std::filesystem::absolute(argv[1]) / "transparent.pfm").string();
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.windowWidth = 129;
  startup.windowHeight = 97;
  ok(api.Startup(&startup));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable(
        (std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  config("upscaler", "off");
  config("denoiser", "off");
  config("taa", "0");
  config("sigma", "0");
  config("sharc", "0");
  config("bounces", "0");
  config("localLights", "0");
  config("bloom", "0");
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  // Captures are read right after the frame that writes them.
  ok(api.SetConfigVariable("rtx.native.asyncPresent", "0"));
  auto &runtime = remix::api::g_runtime;
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1;
  remixapi_CameraEnvironmentEXT sky{};
  sky.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  sky.enabled = 1;
  sky.radiance = {1, 1, 1};
  epoch.pNext = &sky;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  uint64_t id = 100;
  std::vector<remixapi_MeshHandle> meshes;
  std::vector<remixapi_MaterialHandle> materials;
  auto quad = [&](float color, float alpha, float z) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {color, color, color};
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo mi{};
    mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    mi.pNext = &opaque;
    mi.hash = id++;
    remixapi_MaterialHandle mat{};
    ok(api.CreateMaterial(&mi, &mat));
    materials.push_back(mat);
    remixapi_HardcodedVertex v[4]{};
    for (int i = 0; i < 4; ++i) {
      v[i].position[0] = (i == 0 || i == 3) ? -2 : 2;
      v[i].position[1] = i < 2 ? -2 : 2;
      v[i].position[2] = z;
      v[i].color = 0xffffffff;
    }
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = v;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = mat;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = id++;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle mesh{};
    ok(api.CreateMesh(&info, &mesh));
    meshes.push_back(mesh);
    return mesh;
  };
  auto dark = quad(.0625, 1, .6), gray = quad(.25, 1, .6),
       white = quad(1, 1, .6), glass = quad(.25, .5, .4);
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, 0, 1};
  remixapi_LightInfo li{};
  li.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  li.pNext = &distant;
  li.hash = id++;
  li.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
  remixapi_LightHandle light{};
  ok(api.CreateLight(&li, &light));
  remixapi_MeshHandle surface = gray;
  bool transparent = false;
  auto draw = [&](remixapi_MeshHandle mesh, bool alpha) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = uint64_t(mesh);
    identity.rasterVisible = 1;
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.pNext = &identity;
    blend.alphaBlendEnabled = alpha;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &blend;
    instance.mesh = mesh;
    instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i)
      instance.transform.matrix[i][i] = 1;
    ok(api.DrawInstance(&instance));
  };
  auto submit = [&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(light));
    if (surface)
      draw(surface, false);
    if (transparent)
      draw(glass, true);
    ok(api.Present(nullptr));
  };
  auto frame = remix::test::preparedFrame(submit);
  auto settle = [&] {
    for (int i = 0; i < 8; ++i)
      frame();
    return runtime.postProcessingStatistics();
  };
  float reference = 0;
  for (auto visibility : {"raster", "ray"})
    for (auto denoiser : {"off", "reblur-sh"}) {
      config("primaryVisibility", visibility);
      config("denoiser", denoiser);
      for (auto material : {dark, gray, white}) {
        surface = material;
        ++epoch.historyResetEpoch;
        auto stats = settle();
        std::printf("%s %s material %p: exposure %.5f target %.5f\n",
                    visibility, denoiser, material, stats.exposure,
                    stats.target);
        if (!reference)
          reference = stats.target;
        require(std::abs(stats.target - reference) < .015f,
                "Material guide did not meter identical illumination equally");
      }
      surface = white;
      transparent = true;
      ++epoch.historyResetEpoch;
      setenv("REMIX_PATH_LINEAR_CAPTURE", linearPath.c_str(), 1);
      auto stats = settle();
      unsetenv("REMIX_PATH_LINEAR_CAPTURE");
      auto *pfm = std::fopen(linearPath.c_str(), "rb");
      require(pfm, "Missing unexposed transparency capture");
      unsigned width, height;
      float scale;
      require(std::fscanf(pfm, "PF\n%u %u\n%f", &width, &height, &scale) == 3,
              "Invalid PFM");
      std::fgetc(pfm);
      remix::postfx::Meter expectedMeter;
      for (unsigned y = height; y-- > 0;)
        for (unsigned x = 0; x < width; ++x) {
          std::array<float, 3> pixel;
          require(std::fread(pixel.data(), sizeof(float), 3, pfm) == 3,
                  "Short PFM");
          // Authored opacity is tinted: alpha*.25 + (1-alpha)*.25*white.
          expectedMeter.sample(pixel, {.25f, .25f, .25f}, x, y, {width, height},
                               .75f);
        }
      std::fclose(pfm);
      remix::postfx::Exposure expected;
      expected.resolve(expectedMeter, .2f, -16, 2, 1);
      std::printf("Transparency %s %s target %.5f reference %.5f\n", visibility,
                  denoiser, stats.target, expected.target);
      require(
          std::abs(stats.target - expected.target) < .025f,
          "Transparency material composition differs from lighting weights");
      transparent = false;
    }
  surface = nullptr;
  config("autoExposureMaxEv", "24");
  ++epoch.historyResetEpoch;
  auto skyStats = settle();
  const float skyReference = .2f / (.18f * (.025f + .035f + .055f) / 3);
  std::printf("Sky target %.5f reference %.5f\n", skyStats.target,
              skyReference);
  require(std::abs(skyStats.target - skyReference) < .1f,
          "Sky material guide is not white");
  config("autoExposureMaxEv", "2");
  surface = gray;
  config("primaryVisibility", "raster");
  config("denoiser", "reblur-sh");
  config("taa", "1");
  ++epoch.historyResetEpoch;
  for (unsigned i = 0; i < 150; ++i)
    frame();
  auto sequence = [&] {
    remixapi_TracingStatisticsEXT s{.structSize = sizeof(s)};
    ok(remixapi_GetTracingStatisticsEXT(&s));
    return s.sampleSequence;
  };
  auto before = sequence();
  float oldExposure = runtime.postProcessingStatistics().exposure;
  config("autoExposureTarget", "0.05");
  for (unsigned i = 0; i < 40; ++i) {
    frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(sequence() == before, "Exposure edit retraced cached HDR");
  require(runtime.postProcessingStatistics().exposure < oldExposure * .8,
          "Cached HDR exposure did not adapt");
  const auto pipelines = runtime.preparationStatus().pipelines.created;
  config("exposure", "1.4");
  config("autoExposureSpeed", "8");
  settle();
  require(runtime.preparationStatus().pipelines.created == pipelines,
          "Scalar post edit compiled a pipeline");
  failPostImage = true;
  config("bloom", "1");
  settle();
  require(!runtime.postProcessingFailure().empty(),
          "Post allocation failure was not reported");
  require(runtime.postProcessingStatistics().meterFrames > 0,
          "Post failure discarded working exposure");
  config("bloom", "0");
  settle();
  holdPostImage = true;
  config("bloom", "1");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!imageEntered) {
    submit();
    require(std::chrono::steady_clock::now() < deadline,
            "Post preparation worker did not enter allocation");
  }
  const auto presentations = runtime.presentationCount();
  ok(api.SetUIState(REMIXAPI_UI_STATE_ADVANCED));
  for (unsigned i = 0; i < 8; ++i)
    submit();
  require(runtime.presentationCount() >= presentations + 8,
          "Post preparation blocked presentation/F12");
  config("bloom", "0");
  submit(); // supersede the held request
  config("bloom", "1");
  submit(); // only this complete configuration can activate
  holdPostImage = false;
  imageGate.notify_all();
  settle();
  ok(api.SetUIState(REMIXAPI_UI_STATE_NONE));
  require(runtime.postProcessingFailure().empty() &&
              runtime.postProcessingStatistics().bloomFrames,
          "Latest bloom retry failed");
  auto compiled = runtime.preparationStatus().pipelines.created;
  config("bloom", "0");
  config("autoExposure", "0");
  settle();
  config("bloom", "1");
  config("autoExposure", "1");
  settle();
  require(runtime.preparationStatus().pipelines.created == compiled,
          "Effect toggle recompiled pipelines");
  config("autoExposureSpeed", "0.1");
  config("autoExposureTarget", "0.4");
  settle();
  float preserved = runtime.postProcessingStatistics().exposure;
  SDL_SetWindowSize(runtime.window().handle(), 151, 103);
  settle();
  require(std::abs(runtime.postProcessingStatistics().exposure - preserved) <
              .2f,
          "Resize snapped exposure");
  ok(api.SetConfigVariable("rtx.native.vsync", "true"));
  settle();
  require(runtime.postProcessingStatistics().exposure <
              runtime.postProcessingStatistics().target * .9f,
          "VSync snapped exposure");
  ++epoch.historyResetEpoch;
  settle();
  require(std::abs(runtime.postProcessingStatistics().exposure -
                   runtime.postProcessingStatistics().target) < .01f,
          "Camera reset did not snap exposure");
  SDL_SetWindowSize(runtime.window().handle(), 640, 480);
  // Perspective reconstruction meters the display-resolution HDR result while
  // fetching the independently produced material guide at render resolution.
  for (auto &row : camera.projection)
    for (float &value : row)
      value = 0;
  camera.projection[0][0] = 1.5f;
  camera.projection[1][1] = 2;
  camera.projection[2][2] = 100.f / 99.95f;
  camera.projection[2][3] = 1;
  camera.projection[3][2] = -5.f / 99.95f;
  config("bounces", "1");
  config("sigma", "1");
  config("sharcCapacityLog2", "18");
  config("sharc", "1");
  for (auto visibility : {"raster", "ray"})
    for (auto backend : {"fsr31", "sr", "rr"}) {
      config("primaryVisibility", visibility);
      config("upscaler", std::string(backend) == "fsr31" ? "fsr31" : "dlss");
      config("dlssRayReconstruction", std::string(backend) == "rr" ? "1" : "0");
      ++epoch.historyResetEpoch;
      for (unsigned i = 0; i < 32; ++i)
        frame();
      auto stats = runtime.postProcessingStatistics();
      auto reconstruction = runtime.reconstructionStatistics();
      if (!reconstruction.active)
        std::fprintf(stderr, "Inactive %s: %s\n", backend,
                     reconstruction.failure.c_str());
      require(reconstruction.active,
              "Reconstruction inactive during post-processing test");
      require(stats.meterFrames && stats.bloomFrames &&
                  std::isfinite(stats.exposure) && stats.exposure > 0 &&
                  stats.exposure <= 4,
              "Reconstructed HDR metering/bloom failed");
      std::printf(
          "Postfx reconstruction %s %s active=%u exposure=%.4f bytes=%llu\n",
          visibility, backend, unsigned(reconstruction.backend), stats.exposure,
          (unsigned long long)stats.memoryBytes);
      const auto metered = stats.meterFrames;
      config("debugView", "1");
      settle();
      require(runtime.postProcessingStatistics().meterFrames == metered,
              "Diagnostic display adapted exposure");
      config("debugView", "0");
    }
  if (argc > 1) {
    std::filesystem::create_directories(argv[1]);
    auto path = std::filesystem::absolute(argv[1]) / "postfx.png";
    ok(api.RequestPresentedScreenshot(path.c_str()));
    frame();
    frame();
  }
  config("autoExposureTarget", "0.02");
  frame();
  ok(api.SetConfigVariable("rtx.native.backend", "raster"));
  settle();
  remixapi_TracingStatisticsEXT stopped{.structSize = sizeof(stopped)};
  ok(remixapi_GetTracingStatisticsEXT(&stopped));
  require(!stopped.redrawRequested,
          "Inactive post-processing kept requesting redraws");
  for (auto mesh : meshes)
    ok(api.DestroyMesh(mesh));
  for (auto mat : materials)
    ok(api.DestroyMaterial(mat));
  ok(api.DestroyLight(light));
  ok(api.Shutdown());
  std::puts("Postfx runtime material guides, cached frames, failure recovery, "
            "resize and pipeline reuse passed");
}
