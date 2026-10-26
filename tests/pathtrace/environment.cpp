#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
  const bool displayTest = std::getenv("REMIX_TEST_DISPLAY") != nullptr;
  ok(api.SetConfigVariable("rtx.native.pathtrace.samples", "16"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "1"));
  require(api.SetConfigVariable("rtx.native.pathtrace.sky", "2") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid sky toggle accepted");
  uint64_t next = 100;
  auto material = [&](remixapi_Float3D color, float alpha, bool cutout = false) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = cutout ? 4 : 7; // GREATER .5
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = next++;
    remixapi_MaterialHandle handle{};
    ok(api.CreateMaterial(&info, &handle));
    return handle;
  };
  const auto white = material({1, 1, 1}, 1), black = material({0, 0, 0}, 1);
  const auto cutout = material({1, 1, 1}, 0, true), tint = material({.1f, 1, .2f}, .5f);
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1, remixapi_MaterialHandle mat) {
    remixapi_HardcodedVertex vertices[4]{};
    for (int i = 0; i < 4; ++i) {
      vertices[i].position[axis] = plane;
      vertices[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      vertices[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      vertices[i].color = 0xffffffff;
      vertices[i].texcoord[0] = i == 0 || i == 3 ? 0.f : 1.f;
      vertices[i].texcoord[1] = i < 2 ? 0.f : 1.f;
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
  std::vector<remixapi_MeshHandle> sides;
  for (float sign : {-1.f, 1.f}) {
    sides.push_back(quad(0, sign, -1, 1, -1, .5f, black));
    sides.push_back(quad(1, sign, -1, .5f, -1, 1, black));
  }
  const auto back = quad(2, -1, -1, 1, -1, 1, black);
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
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT; blend.pNext = &identity;
    blend.alphaBlendEnabled = mesh == glass;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO; instance.pNext = &blend;
    instance.mesh = mesh; instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i) instance.transform.matrix[i][i] = 1;
    ok(api.DrawInstance(&instance));
  };
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    draw(receiver);
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
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int width, height, channels;
    auto pixels = stbi_load(file.c_str(), &width, &height, &channels, 4);
    require(pixels, "Missing sky screenshot");
    std::array<float, 3> average{};
    for (int y = height / 2 - 4; y < height / 2 + 4; ++y)
      for (int x = width / 2 - 4; x < width / 2 + 4; ++x)
        for (int c = 0; c < 3; ++c) {
          float v = pixels[(y * width + x) * 4 + c] / 255.f;
          average[c] += (displayTest ? v : (std::pow(v, 2.2f))) / 64;
        }
    stbi_image_free(pixels);
    std::printf("%s %s RGB: %.4f %.4f %.4f\n", name.c_str(), displayTest ? "encoded" : "linear",
                average[0], average[1], average[2]);
    return average;
  };
  auto capture = [&](const std::string &name, int frames) {
    // Independent scenes use explicit cuts; live-change checks below do not.
    ++epoch.historyResetEpoch;
    for (int i = 0; i < frames; ++i) frame();
    return readback(name);
  };
  remixapi_DenoisingCapabilitiesEXT denoiseCaps{};
  if (std::getenv("REMIX_TEST_EMISSION")) {
    auto config = [&](const char *key, const char *value) {
      ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.")+key).c_str(),value));
    };
    if (std::getenv("REMIX_TEST_EMISSION_STABILITY")) {
      config("stabilizeEmissiveLighting","1");
      config("emissiveSolidAngleSampling","1");
    }
    config("sky","0"); config("sharc","0"); config("bounces","0");
    config("localRange","0"); config("localGuiding","0"); config("localSamples","4");
    if (std::getenv("REMIX_TEST_EMISSION_FSR")) {
      for (auto &row : camera.projection) for (float &v : row) v=0;
      camera.projection[0][0]=camera.projection[1][1]=2;
      camera.projection[2][2]=100.f/99.95f; camera.projection[2][3]=1;
      camera.projection[3][2]=-5.f/99.95f;
      config("upscaler","fsr31"); config("fsrQuality","native-aa");
    }
    uint8_t pixels[] = {128,0,0,255};
    remixapi_TextureInfo tex{};
    tex.sType=REMIXAPI_STRUCT_TYPE_TEXTURE_INFO; tex.hash=500; tex.width=tex.height=1;
    tex.format=REMIXAPI_FORMAT_R8G8B8A8_SRGB; tex.data=pixels; tex.dataSize=4;
    remixapi_TextureHandle textureHandle{}; ok(api.CreateTexture(&tex,&textureHandle));
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.opacityConstant=1; opaque.alphaTestType=7;
    remixapi_MaterialInfo info{};
    info.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO; info.hash=501; info.pNext=&opaque;
#if defined(_WIN32)
    info.emissiveTexture=L"0x1f4";
#else
    info.emissiveTexture="0x1f4";
#endif
    info.emissiveIntensity=2; info.emissiveColorConstant={1,1,1};
    remixapi_MaterialHandle emitterMaterial{}; ok(api.CreateMaterial(&info,&emitterMaterial));
    auto emitter=quad(2,-1,-.5f,.5f,-.5f,.5f,emitterMaterial);
    auto visible=quad(2,.25f,-1,1,-1,1,emitterMaterial);
    auto blocker=quad(2,-.5f,-1,1,-1,1,black);
    const double radiance=2*std::pow(128./255,2.2);
    double expected=0;
    for (int y=0;y<128;++y) for (int x=0;x<128;++x) {
      double a=(x+.5)/128-.5,b=(y+.5)/128-.5,d2=a*a+b*b+2.25;
      expected+=radiance*2.25/(3.141592653589793*d2*d2*128*128);
    }
    for (const char *primary : {"raster","ray"}) for (bool filtered : {false,true}) {
      config("primaryVisibility",primary); config("accumulate",filtered?"0":"1");
      std::string prefix=std::string(primary)+(filtered?"-filtered-":"-raw-");
      enclosure={emitter};
      auto lit=capture(prefix+"emission-light",filtered?80:32);
      require(std::abs(lit[0]-expected)<.018 && lit[1]<.004 && lit[2]<.004,"Emissive-map irradiance/color incorrect");
      remixapi_LocalLightStatisticsEXT stats{}; stats.structSize=sizeof(stats);
      ok(remixapi_GetLocalLightStatisticsEXT(&stats));
      require(stats.activeLights==2,"Emissive quad did not create two sampled triangles");
      config("bounces","1");
      auto bounced=capture(prefix+"emission-one-bounce",filtered?80:32);
      require(std::abs(bounced[0]-expected)<.018,"Secondary emitter hit double-counted next-event lighting");
      config("bounces","0");
      enclosure.push_back(blocker);
      auto blocked=capture(prefix+"emission-blocked",filtered?80:32);
      require(blocked[0]<.004,"Opaque wall leaked emissive light");
      enclosure={visible};
      auto glow=capture(prefix+"emission-visible",filtered?80:32);
      require(std::abs(glow[0]-radiance)<.015 && glow[1]<.004,"Visible emission lost energy or was shadowed");
      config("localLights","0");
      auto glowOff=capture(prefix+"emission-visible-local-off",8);
      require(std::abs(glowOff[0]-radiance)<.015,"Local toggle disabled visible emission");
      enclosure={emitter};
      require(capture(prefix+"emission-local-off",16)[0]<.004,"Local toggle failed to disable emission lighting");
      config("localLights","1");
      config("emissiveLights","0");
      require(capture(prefix+"emissive-casting-off",16)[0]<.004,"Emissive toggle retained cast light");
      enclosure={visible};
      require(std::abs(capture(prefix+"emissive-visible-casting-off",16)[0]-radiance)<.015,
              "Emissive casting toggle hid the visible map");
      config("emissiveLights","1");
      remixapi_MaterialInfoEmissionEXT policy{};
      policy.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT;
      policy.suppressLightGeneration=1; opaque.pNext=&policy;
      ok(api.CreateMaterial(&info,&emitterMaterial));
      require(std::abs(capture(prefix+"visible-only-emission",16)[0]-radiance)<.015,
              "Per-material suppression hid the visible emissive map");
      ok(remixapi_GetLocalLightStatisticsEXT(&stats));
      require(stats.activeLights==0,"Visible-only material generated duplicate area lights");
      enclosure={emitter};
      require(capture(prefix+"visible-only-receiver",16)[0]<.004,
              "Visible-only material still cast light");
      opaque.pNext=nullptr; ok(api.CreateMaterial(&info,&emitterMaterial));
    }
    // A visible red/black/green/black map must retain detail even though its
    // emitter triangles still use CPU-averaged radiance.
    uint8_t pattern[] = {128,0,0,255, 0,0,0,255, 0,128,0,255, 0,0,0,255};
    tex.width=4; tex.data=pattern; tex.dataSize=sizeof(pattern);
    ok(api.CreateTexture(&tex,&textureHandle)); ok(api.CreateMaterial(&info,&emitterMaterial));
    auto patterned=quad(2,.25f,-.125f,.125f,-.125f,.125f,emitterMaterial);
    for (auto &row : camera.projection) for (float &v : row) v=0;
    camera.projection[0][0]=camera.projection[1][1]=2;
    camera.projection[2][2]=100.f/99.95f; camera.projection[2][3]=1;
    camera.projection[3][2]=-5.f/99.95f;
    enclosure={patterned};
    for (const char *primary : {"raster","ray"}) for (bool filtered : {false,true}) {
      config("primaryVisibility",primary); config("accumulate",filtered?"0":"1");
      auto name=std::string(primary)+(filtered?"-filtered-map-detail":"-raw-map-detail");
      capture(name,filtered?80:32);
      int w,h,n; auto data=stbi_load((directory/(name+".png")).string().c_str(),&w,&h,&n,4);
      require(data,"Missing emission detail screenshot");
      for (unsigned stripe=0;stripe<4;++stripe) for (unsigned c=0;c<3;++c) {
        unsigned x=(2*stripe+1)*w/8;
        float value=data[((h/2)*w+x)*4+c]/255.f;
        value=std::pow(value, 2.2f);
        double expectedPixel=(stripe==0&&c==0)||(stripe==2&&c==1)?radiance:0;
        require(std::abs(value-expectedPixel)<.02,"Visible emissive map flattened or sampled incorrectly");
      }
      stbi_image_free(data);
    }
    tex.width=1; tex.data=pixels; tex.dataSize=sizeof(pixels);
    pixels[0]=0; ok(api.CreateTexture(&tex,&textureHandle)); ok(api.CreateMaterial(&info,&emitterMaterial));
    require(capture("emission-texture-removed",80)[0]<.004,"Updated black map retained stale light");
    remixapi_LocalLightStatisticsEXT stats{}; stats.structSize=sizeof(stats);
    ok(remixapi_GetLocalLightStatisticsEXT(&stats)); require(stats.activeLights==0,"Black map retained active emitters");
    ok(api.Shutdown()); std::puts("Emissive map lighting, occlusion, visible glow, controls and updates passed"); return 0;
  }
  if (displayTest) {
    for (auto &row : camera.projection) for (float &v : row) v = 0;
    camera.projection[0][0] = camera.projection[1][1] = 2;
    camera.projection[2][2] = 100.f / 99.95f;
    camera.projection[2][3] = 1;
    camera.projection[3][2] = -5.f / 99.95f;
    auto config = [&](const char *key, const char *value) {
      ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
    };
    require(api.SetConfigVariable("rtx.native.pathtrace.prism", "0") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "The Prism tone curve can be turned off");
    require(api.SetConfigVariable("rtx.native.pathtrace.gamma22", "0") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "The gamma 2.2 output encode can be turned off");
    config("sharc", "0");
    config("denoiser", "off");
    config("sigma", "0");
    config("taa", "0");
    for (const char *primary : {"raster", "ray"}) for (bool fsr : {false, true}) {
      config("primaryVisibility", primary);
      config("upscaler", fsr ? "fsr31" : "off");
      config("taa", fsr ? "1" : "0");
      config("fsrQuality", "native-aa");
      for (float value : {0.f, .001f, .18f, 1.f, 8.f}) {
        environment.radiance = {value, value, value};
        auto name = std::string(primary) + (fsr ? "-fsr-" : "-native-") + std::to_string(value);
        auto pixel = capture(name, fsr ? 24 : 4);
        // Grey keeps its Prism primaries: the curve applies to the value.
        double mapped = value;
        if (value > .18) {
          const double t = (double(value) - .18) / .82;
          mapped = .18 + .82 * t / (1 + t * std::exp(-.01 / (t * t)));
        }
        mapped = std::clamp(mapped, 0.0, 1.0);
        double expected = std::pow(mapped, 1.0 / 2.2);
        for (float channel : pixel)
          require(std::abs(channel - expected) < .012, "Prism/output transfer disagrees with reference");
        if (fsr) {
          require(!temporal().activeTaa, "Display test fell back from FSR to TAA");
        }
      }
      const remixapi_Float3D colors[] = {{4,.1f,.02f}, {.1f,4,.1f}, {.02f,.1f,4}, {.25f,.5f,.75f}};
      // The curve evaluated independently in its primaries (display.glsl's
      // matrices); the UNORM target clips the green primary's 1.006 to one.
      const float expectedColors[][3] = {{.98657904f,.42050823f,.23460005f}, {.44606123f,1,.25640359f},
                                         {.30105654f,.4615924f,.9078918f}, {.27017254f,.42137076f,.52559016f}};
      for (int i = 0; i < 4; ++i) {
        environment.radiance = colors[i];
        auto pixel = capture(std::string(primary) + (fsr ? "-fsr-color-" : "-native-color-") + std::to_string(i), fsr ? 24 : 4);
        for (int c = 0; c < 3; ++c)
          require(std::abs(pixel[c] - std::pow(expectedColors[i][c], 1.f / 2.2f)) < .012f,
                  "Prism colored primary/reference mismatch");
      }
    }
    ok(api.Shutdown());
    std::puts("Prism and gamma 2.2 GPU reference values passed for native/FSR and both visibility paths");
    return 0;
  }
  denoiseCaps.structSize = sizeof(denoiseCaps);
  ok(remixapi_GetDenoisingCapabilitiesEXT(&denoiseCaps));
  for (const char *primary : {"raster", "ray"}) for (bool filtered : {false, true}) {
    ok(api.SetConfigVariable("rtx.native.pathtrace.primaryVisibility", primary));
    ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", filtered ? "0" : "1"));
    const std::string prefix = std::string(primary) + (filtered ? "-nrd-taa-" : "-raw-");
    const int frames = filtered ? 48 : 16;
    environment.radiance = {.25f, .5f, .75f}; enclosure.clear();
    auto exposed = capture(prefix + "exposed", frames);
    if (filtered)
      require(temporal().activeDenoiser == denoiseCaps.reblurShSupported &&
                  temporal().activeTaa == denoiseCaps.taaSupported,
              "Supported temporal filters failed to activate");
    for (int c = 0; c < 3; ++c)
      require(std::abs(exposed[c] - .25f * (c + 1)) < .09f, "Sky radiance/color transfer incorrect");
    enclosure = sides; enclosure.push_back(back);
    auto closed = capture(prefix + "closed", frames);
    require(closed[0] < .005f && closed[1] < .005f && closed[2] < .005f, "Closed room leaked sky");
    enclosure = sides; enclosure.insert(enclosure.end(), opening.begin(), opening.end());
    auto opened = capture(prefix + "opening", frames);
    require(opened[1] > .02f && opened[1] < exposed[1] * .7f, "Opening did not admit bounded sky");
    enclosure = sides;
    auto uncovered = capture(prefix + "uncovered", frames);
    enclosure.push_back(holes);
    auto cut = capture(prefix + "cutout", frames);
    require(std::abs(cut[1] - uncovered[1]) < .04f, "Rejected alpha cutout blocked sky");
    enclosure.back() = glass;
    auto tinted = capture(prefix + "tinted", frames);
    require(tinted[1] > .015f && tinted[1] < uncovered[1] * .8f && tinted[0] < tinted[1] * .2f &&
                tinted[2] < tinted[1] * .5f, "Tinted transmission lost sky attenuation/color");
    enclosure.clear();
    environment.enabled = 0;
    auto disabled = capture(prefix + "disabled", frames);
    require(disabled[2] < .005f, "Disabled environment illuminated receiver");
    environment.enabled = 1;
    ok(api.SetConfigVariable("rtx.native.pathtrace.sky", "0"));
    auto toggled = capture(prefix + "toggle-off", frames);
    require(toggled[2] < .005f, "Sky toggle failed");
    ok(api.SetConfigVariable("rtx.native.pathtrace.sky", "1"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "0"));
    auto zero = capture(prefix + "zero-bounce", frames);
    require(zero[2] < .005f, "Zero indirect bounces admitted sky");
    ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "1"));
    frame(); frame();
    const auto resets = temporal().historyResets;
    auto previous = tracing();
    for (int i = 0; i < 12; ++i) {
      environment.radiance.x = .1f + i * .01f;
      light.radiance = {.01f * i, .01f * i, .01f * i};
      distant.direction = {0, -.01f * i, 1};
      ok(api.UpdateLightDefinition(sun, &light)); frame();
      auto now = tracing();
      require(now.sceneGeneration > previous.sceneGeneration, "Sky/sun update reused cached world");
      if (filtered) require(temporal().historyResets == resets, "Time/weather reset global history");
      else require(now.accumulationCount == 1, "Sky/sun update retained progressive accumulation");
      previous = now;
    }
    light.radiance = {}; ok(api.UpdateLightDefinition(sun, &light));
    frame(); frame();
    previous = tracing();
    // A change to sky alone must invalidate progressive/cached output too.
    environment.radiance = {.7f, .2f, .1f}; frame();
    require(tracing().sceneGeneration > previous.sceneGeneration, "Sky-only change not detected");
    if (!filtered) require(tracing().accumulationCount == 1, "Sky-only change retained accumulation");
    for (int i = 0; i < 128 && (!filtered ? i < frames : temporal().redrawRequested); ++i) frame();
    auto updated = readback(prefix + "live-sky-change");
    // NRD retains history for live changes. Require at least 75% of this abrupt
    // color transition during its advertised warmup, rather than a history reset.
    const float before[] = {.21f, .5f, .75f}, after[] = {.7f, .2f, .1f};
    for (int c = 0; c < 3; ++c)
      require(std::abs(updated[c] - after[c]) < .25f * std::abs(before[c] - after[c]) + .005f,
              "History did not respond to live sky radiance");
    if (filtered) require(temporal().historyResets == resets, "Sky-only change reset history");
  }
  ok(api.Shutdown());
  std::puts("Sky illumination, occlusion, transmission, controls and temporal checks passed");
}
