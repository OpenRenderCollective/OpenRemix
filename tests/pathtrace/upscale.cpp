#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
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
void ok(remixapi_ErrorCode code) {
  if (code != REMIXAPI_ERROR_CODE_SUCCESS) std::fprintf(stderr, "Upscaler API error %u\n", unsigned(code));
  require(code == REMIXAPI_ERROR_CODE_SUCCESS, "Upscaler API failure");
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
  startup.windowWidth = std::getenv("REMIX_TEST_BENCHMARK") ? 1920 : 640;
  startup.windowHeight = std::getenv("REMIX_TEST_BENCHMARK") ? 1080 : 512;
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
  const char *ngxMode = std::getenv("REMIX_TEST_NGX");
  const bool rr = ngxMode && std::string(ngxMode) != "sr";
  const char *qualityKey = ngxMode ? "dlssQuality" : "fsrQuality";
  auto diagnostics = [&] {
    remixapi_ReconstructionDiagnosticsEXT result{}; result.structSize = sizeof(result);
    ok(remixapi_GetReconstructionDiagnosticsEXT(&result)); return result;
  };
  if (ngxMode) {
    auto d = diagnostics();
    std::printf("NGX capabilities %u: %s\n", d.capabilities, d.fallbackReason);
    config("dlssRayReconstruction", rr ? "1" : "0");
  }
  config("sharc", "0");
  config("sky", "0");
  config("sigma", "0");
  config("localRange", "16");
  config("localIntensity", "1");
  uint64_t next = 1;
  auto material = [&](remixapi_Float3D color, float alpha) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = 7;
    if (std::getenv("REMIX_TEST_PBR")) {
      opaque.roughnessConstant = .5f;
      opaque.metallicConstant = .35f;
    }
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = next++;
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
  // Two parallel receivers share a tangential guide cell. The front plane
  // lies exactly on a cell boundary. The partition makes the many bright
  // sources useful only on the rear plane, so a wrong cell matters visibly.
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
  sphere.radius = .04f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  std::vector<remixapi_LightHandle> lights(std::getenv("REMIX_TEST_RR_DENSE") ? 64 : 4);
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
  auto capture = [&](const std::string &name, int radius = 12) {
    auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, n;
    auto data = stbi_load(file.c_str(), &w, &h, &n, 4);
    require(data, "Torch capture unavailable");
    Image result;
    float depth = 192.f + camera.view[3][2];
    int px = int(
        (1 + (192.5f + camera.view[3][0]) * camera.projection[0][0] / depth) *
        .5f * w);
    for (int y = h / 2 - radius; y < h / 2 + radius; ++y)
      for (int x = px - radius; x < px + radius; ++x) {
        std::array<float, 3> pixel{};
        for (int c = 0; c < 3; ++c) {
          float v = data[(y * w + x) * 4 + c] / 255.f;
          pixel[c] = std::pow(v, 2.2f);
        }
        result.push_back(pixel);
      }
    stbi_image_free(data);
    return result;
  };

  auto rear = quad(2, 193.f, 193.f, 197.f, 63.f, 67.f, white);
  auto divider = quad(0, 193.05f, 63.f, 67.f, 189.f, 193.1f, black);
  meshes = {receiver, rear, divider};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      camera.projection[i][j] = 0;
  camera.projection[0][0] = 1.6f;
  camera.projection[1][1] = 2.f;
  camera.projection[2][2] = 100.f / 99.95f;
  camera.projection[2][3] = 1.f;
  camera.projection[3][2] = -5.f / 99.95f;
  camera.view[3][0] = -193.f;
  camera.view[3][1] = -65.f;
  for (unsigned i = 0; i < lights.size(); ++i) {
    sphere.position = i == 0 ? remixapi_Float3D{192.4f, 65.f, 190.8f}
                             : remixapi_Float3D{193.6f, 65.f, 190.8f};
    sphere.radius = .08f;
    light.radiance = {20, 10, 4};
    ok(api.UpdateLightDefinition(lights[i], &light));
  }
  config("bounces", "0");
  config("denoiser", "off");
  config("taa", "1");
  config("localSamples", "1");
  config("localGuiding", "1");
  config("accumulate", "0");
  camera.view[3][2] = -188.f;
  config("localSamples", "4");
  for (const auto &entry : std::vector<std::pair<const char *, const char *>>{
           {"upscaler", "fsr4"},
           {"fsrQuality", "potato"},
           {"dlssQuality", "potato"}, {"dlssRayReconstruction", "2"}, {"dlssRayReconstruction", "true"},
           {"fsrSharpness", "nan"},
           {"fsrSharpness", "-0.1"},
           {"fsrSharpness", "1.1"}})
    require(api.SetConfigVariable(
                (std::string("rtx.native.pathtrace.") + entry.first).c_str(),
                entry.second) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "Malformed FSR config accepted");
  auto stats = [&] {
    remixapi_DenoisingStatisticsEXT value{};
    value.structSize = sizeof(value);
    ok(remixapi_GetDenoisingStatisticsEXT(&value));
    return value;
  };
  auto mean = [](const Image &image) {
    double sum = 0;
    for (auto pixel : image)
      sum += .2126 * pixel[0] + .7152 * pixel[1] + .0722 * pixel[2];
    return sum / image.size();
  };
  if (ngxMode) {
    // Device/SDK feature discovery now runs with the requested configuration.
    config("upscaler", "dlss");
    frame();
  }
  const auto ngxCaps = diagnostics().capabilities;
  const bool injected = std::getenv("REMIX_NGX_FAULT") != nullptr;
  const bool fallback = std::getenv("REMIX_TEST_FAULT") != nullptr || (ngxMode && !(ngxCaps & (rr ? 4 : 2))) || injected;
  if (std::getenv("REMIX_TEST_CAMERA_RANGE")) {
    if (fallback) { ok(api.Shutdown()); return 77; }
    config("upscaler", ngxMode ? "dlss" : "fsr31");
    config(qualityKey, "balanced");
    remixapi_CameraInfoParameterizedEXT p{};
    p.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT; p.pNext = &epoch;
    p.aspect = 1.25f; p.nearPlane = .01f; p.farPlane = 100000; p.fovYInDegrees = 60;
    const float scale = 1 / std::tan(p.fovYInDegrees * .00872664625997165f);
    for (auto &column : camera.projection) for (float &v : column) v = 0;
    camera.projection[0][0] = scale / p.aspect; camera.projection[1][1] = scale;
    camera.projection[2][2] = p.farPlane / (p.farPlane - p.nearPlane);
    camera.projection[2][3] = 1;
    camera.projection[3][2] = -p.nearPlane * camera.projection[2][2];
    for (bool parameterized : {false, true}) {
      camera.pNext = parameterized ? static_cast<void *>(&p) : &epoch;
      remixapi_ReconstructionDiagnosticsEXT before{};
      for (unsigned tick = 0; tick < 128; ++tick) {
        const float angle = .01f * tick, c = std::cos(angle), s = std::sin(angle);
        p.position = {1024 + .3f * tick, -8192 + .7f * tick, 4096 - .1f * tick};
        p.right = {c, 0, -s}; p.up = {0, 1, 0}; p.forward = {s, 0, c};
        camera.view[0][0] = c; camera.view[0][2] = s;
        camera.view[2][0] = -s; camera.view[2][2] = c;
        camera.view[3][0] = -c * p.position.x + s * p.position.z;
        camera.view[3][1] = -p.position.y;
        camera.view[3][2] = -s * p.position.x - c * p.position.z;
        frame();
        const auto d = diagnostics();
        if (!tick) before = d;
        if (d.activeBackend != (ngxMode ? (rr ? 3u : 2u) : 1u))
          std::fprintf(stderr, "Camera frame %u (parameterized %d): %s\n", tick, parameterized, d.fallbackReason);
        require(d.activeBackend == (ngxMode ? (rr ? 3u : 2u) : 1u), "Moving camera disabled reconstruction");
        require(d.historyResets == before.historyResets && d.failures == before.failures,
                "Moving camera restarted reconstruction history");
        require(d.renderWidth == before.renderWidth && d.renderHeight == before.renderHeight &&
                    d.evaluatedFrames == before.evaluatedFrames + tick,
                "Moving camera rebuilt or skipped reconstruction");
      }
    }
    std::puts("Wide-range camera translation and rotation retained reconstruction for both camera APIs");
    ok(api.Shutdown()); return 0;
  }
  if (std::getenv("REMIX_TEST_RR_DENSE")) {
    if (!(ngxCaps & 4)) { ok(api.Shutdown()); return 77; }
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, .5f, .2f}; opaque.opacityConstant = 1; opaque.alphaTestType = 7;
    remixapi_MaterialInfo emitter{}; emitter.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    emitter.hash = next++; emitter.pNext = &opaque; emitter.emissiveIntensity = 3;
    emitter.emissiveColorConstant = {1, .5f, .2f};
    remixapi_MaterialHandle handle{}; ok(api.CreateMaterial(&emitter, &handle));
    meshes.push_back(quad(1, 66.5f, 190, 192, 191, 193, handle));
    for (unsigned i = 0; i < lights.size(); ++i) {
      sphere.position = {191.4f + float(i % 8) * .18f, 64.4f + float(i / 8) * .18f, 190.8f};
      light.radiance = {1.25f, .625f, .25f}; ok(api.UpdateLightDefinition(lights[i], &light));
    } config("sigma", "1");
    config("adaptiveLocalSampling", "0"); config("localSamples", "1");
    config("dlssQuality", "quality"); config("denoiser", "reblur-sh"); config("bounces", "1");
    for (const char *primary : {"raster", "ray"}) {
      config("primaryVisibility", primary);
      config("upscaler", "off"); config("samples", "16"); config("accumulate", "1");
      for (int i = 0; i < 128; ++i) { renderSequence = 0; frame(); }
      const double reference = mean(capture(std::string(primary) + "-dense-reference"));
      config("samples", "1"); config("accumulate", "0"); config("upscaler", "auto");
      for (const char *family : {"0", "1", "2", "3"}) {
        config("separateLocalLighting", family);
        for (int i = 0; i < 128; ++i) frame();
        const double reconstructed = mean(capture(std::string(primary) + "-dense-family-" + family));
        require(diagnostics().activeBackend == 3, "Dense lighting RR unavailable");
        require(reconstructed > reference * .8 && reconstructed < reference * 1.2,
                "RR mixed lighting energy differs from accumulated reference");
        std::printf("%s family %s: reference %.6f, RR %.6f\n", primary, family, reference, reconstructed);
      }
      config("sharc", "1"); for (int i = 0; i < 64; ++i) frame();
      remixapi_SharcStatisticsEXT sharc{}; sharc.structSize = sizeof(sharc);
      ok(remixapi_GetSharcStatisticsEXT(&sharc)); require(sharc.active, "RR disabled SHARC");
      config("sharc", "0");
    }
    config("upscaler", "off"); for (int i = 0; i < 16; ++i) frame();
    require(stats().activeDenoiser && stats().activeTaa, "Saved NRD/TAA settings did not resume");
    ok(api.Shutdown()); return 0;
  }
  if (std::getenv("REMIX_TEST_BENCHMARK")) {
    config("denoiser", "reblur-sh"); config("sigma", "1"); config("bounces", "2");
    config("localSamples", "1"); config("samples", "1"); config("fsrQuality", "quality"); config("dlssQuality", "quality");
    std::puts("mode,total_ms,present_ms,trace_ms,build_ms,reconstruct_ms,reconstruction_bytes,denoise_bytes,acceleration_bytes,render_width,render_height");
    for (const char *mode : {"off", "fsr31", "dlss", "auto"}) {
      config("dlssRayReconstruction", std::string(mode)=="auto"?"1":"0"); config("upscaler",mode);
      camera.view[3][0] = -193.f;
      for (unsigned i=0;i<180;++i) frame();
      const auto begin = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      double total=0,present=0,trace=0,build=0,reconstruct=0;
      for (unsigned i=0;i<360;++i) {
        camera.view[3][0]=-193.f+.08f*std::sin(float(i)*.025f);
        const auto start=std::chrono::steady_clock::now(); frame();
        total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        remixapi_FramePerformanceEXT cpu{};cpu.structSize=sizeof(cpu);ok(remixapi_GetFramePerformanceEXT(&cpu));
        remixapi_TracingStatisticsEXT gpu{};gpu.structSize=sizeof(gpu);ok(remixapi_GetTracingStatisticsEXT(&gpu));
        present+=cpu.presentMilliseconds;trace+=gpu.gpuTraceMilliseconds;build+=gpu.gpuBuildMilliseconds;
        reconstruct+=diagnostics().gpuMilliseconds;
      }
      const auto end = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      std::printf("BENCH_INTERVAL,%s,%lld,%lld\n", mode, static_cast<long long>(begin), static_cast<long long>(end));
      const auto d=diagnostics(); const auto n=stats();
      remixapi_TracingStatisticsEXT gpu{};gpu.structSize=sizeof(gpu);ok(remixapi_GetTracingStatisticsEXT(&gpu));
      std::printf("%s,%.4f,%.4f,%.4f,%.4f,%.4f,%llu,%llu,%llu,%u,%u\n",mode,total/360,present/360,trace/360,build/360,reconstruct/360,
        (unsigned long long)d.memoryBytes,(unsigned long long)n.memoryBytes,(unsigned long long)gpu.accelerationBytes,d.renderWidth,d.renderHeight);
      capture(std::string("benchmark-")+mode); std::fflush(stdout);
    }
    ok(api.Shutdown()); return 0;
  }
  if (std::getenv("REMIX_TEST_NGX_SMOKE")) {
    config("upscaler", "auto");
    for (int i = 0; i < 12; ++i) frame();
    const auto d = diagnostics();
    std::printf("Active %u, %ux%u -> %ux%u, %llu evaluations, %.3f ms, %s\n", d.activeBackend,
      d.renderWidth,d.renderHeight,d.displayWidth,d.displayHeight,(unsigned long long)d.evaluatedFrames,d.gpuMilliseconds,d.fallbackReason);
    capture("ngx-smoke");
    require(d.activeBackend == 3 && d.evaluatedFrames > 0, "RR did not initialize and evaluate");
    ok(api.Shutdown()); return 0;
  }
  for (const char *primary : {"raster", "ray"}) {
    config("primaryVisibility", primary);
    config("upscaler", "off");
    config("denoiser", "off");
    for (int i = 0; i < 64; ++i)
      frame();
    double reference = mean(capture(std::string(primary) + "-native"));
    require(reference > .01, "Unlit FSR reference");
    config("upscaler", ngxMode ? (std::string(ngxMode) == "auto" ? "auto" : "dlss") : "fsr31");
    for (const char *quality : {"native-aa", "quality", "balanced",
                                "performance", "ultra-performance"}) {
      config(qualityKey, quality);
      for (int nrd = 0; nrd < 2; ++nrd) {
        config("denoiser", nrd ? "reblur-sh" : "off");
        config("fsrSharpness", nrd ? "0.3" : "0");
        for (int i = 0; i < 64; ++i)
          frame();
        auto before = stats();
        const auto d = diagnostics();
        if (ngxMode) {
          std::printf("%s %s active %u (%s)\n", primary, quality, d.activeBackend, d.fallbackReason);
          uint32_t expected = (ngxCaps & (rr ? 4 : 2)) ? (rr ? 3 : 2) : (rr && (ngxCaps & 2) ? 2 : 0);
          const std::string fault = std::getenv("REMIX_NGX_FAULT") ? std::getenv("REMIX_NGX_FAULT") : "";
          if (fault == "rr-create" || fault == "rr-evaluate") expected = (ngxCaps & 2) ? 2 : 0;
          if (fault == "sr-create" || fault == "sr-evaluate" || fault == "allocation") expected = 0;
          require(d.activeBackend == expected, "Incorrect NGX active backend/fallback");
          if (expected) require(d.evaluatedFrames > 0, "No NGX evaluations");
          if (injected || !expected) require(d.fallbackReason[0], "Missing NGX fallback reason");
          if (expected == 3) require(!before.activeDenoiser && !before.activeTaa, "RR ran NRD/TAA");
          require(bool(before.activeTaa) == (expected == 0), "Incorrect native TAA fallback");
        } else require(bool(before.activeTaa) == fallback, "FSR did not replace TAA (or native fallback failed)");
        double low = 100, high = 0;
        const auto failureCount = diagnostics().failures;
        for (int i = 0; i < 12; ++i) {
          camera.view[3][2] = -188.f - .08f * std::sin(float(i) * .6f);
          double value =
              mean(capture(std::string(primary) + "-" + quality + "-" +
                           std::to_string(nrd) + "-" + std::to_string(i)));
          low = std::min(low, value);
          high = std::max(high, value);
        }
        require(diagnostics().failures == failureCount, "Repeated failed NGX initialization");
        require(stats().historyResets == before.historyResets,
                "FSR camera motion reset temporal history");
        std::printf("%s %s NRD %d: reference %.6f, motion %.6f..%.6f\n",
                    primary, quality, nrd, reference, low, high);
        require(low > reference * .7 && high < reference * 1.3,
                "FSR lost torch energy or created broad motion pulses");
        camera.view[3][2] = -188.f;
      }
      if (fallback)
        break;
    }
    if (fallback)
      break;
  }
  if (!fallback) {
    // Colored transparent sheets have background motion but need responsive
    // composition history. They must retain their tint when moved/revealed.
    config(qualityKey, "quality");
    config("localSamples", "1");
    remixapi_MaterialInfoTranslucentEXT glassInfo{};
    glassInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    glassInfo.transmittanceColor = {.15f, .65f, .9f};
    remixapi_MaterialInfo glassMat{};
    glassMat.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    glassMat.pNext = &glassInfo;
    glassMat.hash = next++;
    remixapi_MaterialHandle glassHandle{};
    ok(api.CreateMaterial(&glassMat, &glassHandle));
    auto glass = quad(2, 191.f, 190.f, 198.f, 62.f, 68.f, glassHandle);
    meshes.push_back(glass);
    for (int i = 0; i < 64; ++i)
      frame();
    auto tinted = capture("tinted-sheet");
    meshes.pop_back();
    for (int i = 0; i < 16; ++i)
      frame();
    auto revealed = capture("revealed-wall");
    double blue = 0, red = 0;
    for (size_t i = 0; i < tinted.size(); ++i) {
      red += tinted[i][0] / std::max(revealed[i][0], .001f);
      blue += tinted[i][2] / std::max(revealed[i][2], .001f);
    }
    require(blue > red * 2, "FSR lost colored transmission");
    int count = 0;
    auto windows = SDL_GetWindows(&count);
    require(count > 0, "Missing test window");
    auto window = windows[0];
    SDL_free(windows);
    require(SDL_SetWindowSize(window, 800, 600), "Resize FSR window");
    for (int i = 0; i < 32; ++i)
      frame();
    capture("resized");
    config("accumulate", "1");
    for (int i = 0; i < 4; ++i) {
      renderSequence = 0;
      frame();
    }
    remixapi_TracingStatisticsEXT tracing{};
    tracing.structSize = sizeof(tracing);
    ok(remixapi_GetTracingStatisticsEXT(&tracing));
    require(tracing.accumulationCount >= 2, "FSR blocked raw accumulation");
    config("accumulate", "0");
    for (int i = 0; i < 16; ++i)
      frame();
    require(!stats().activeTaa, "FSR did not resume after accumulation");
    config("upscaler", "off");
    for (int i = 0; i < 16; ++i)
      frame();
    require(stats().activeTaa, "Native TAA did not resume");
  }
  ok(api.Shutdown());
  std::puts(
      "FSR presets, motion, transparency, resizing and native fallback passed");
}
