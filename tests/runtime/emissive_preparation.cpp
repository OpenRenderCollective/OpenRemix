#include "remix-native/scene.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
void checked(remixapi_ErrorCode result, int line) {
  if (result) {
    std::fprintf(stderr, "API failure %u at line %d\n", unsigned(result), line);
    std::exit(1);
  }
}
#define ok(call) checked(call, __LINE__)
}

int main() {
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = 320; startup.windowHeight = 240;
#endif
  ok(api.Startup(&startup));
  remixapi_BackendCapabilitiesEXT capabilities{};
  capabilities.structSize = sizeof(capabilities);
  ok(remixapi_GetBackendCapabilitiesEXT(&capabilities));
  remixapi_DenoisingCapabilitiesEXT denoising{};
  denoising.structSize = sizeof(denoising);
  ok(remixapi_GetDenoisingCapabilitiesEXT(&denoising));
  if (!(capabilities.supportedBackends & 2) || !denoising.taaSupported) {
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  ok(api.SetConfigVariable("rtx.native.lowLatency", "0"));
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  for (const auto *setting : {"upscaler", "denoiser"})
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + setting).c_str(), "off"));
  for (const auto *setting : {"taa", "sigma", "localLights", "emissiveLights"})
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + setting).c_str(), "1"));
  for (const auto *setting : {"sharc", "restir", "regir", "localGuiding"})
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + setting).c_str(), "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.separateLocalLighting", "1"));
  SampleScene scene;
  ok(scene.create(api));
  bool showEmitter = false;
  bool tracingActive = false;
  remixapi_MaterialHandle material{};
  remixapi_MeshHandle mesh{};
  auto frame = [&] {
    ok(api.PumpEvents());
    remixapi_WindowState window{};
    ok(api.GetWindowState(&window));
    ok(scene.draw(api, 0, window.drawableWidth, window.drawableHeight));
    if (showEmitter) {
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.mesh = mesh; instance.doubleSided = 1;
      instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
      ok(api.DrawInstance(&instance));
    }
    // Must remain successful on every frame, including while new pipelines compile.
    ok(api.Present(nullptr));
    ok(remixapi_GetBackendCapabilitiesEXT(&capabilities));
    tracingActive = capabilities.activeBackend == 1 && capabilities.requestedBackend == 1;
    remixapi_PreparationStatisticsEXT status{};
    status.structSize = sizeof(status);
    ok(remixapi_GetPreparationStatisticsEXT(&status));
    require(!status.failed && !status.pipelinesFailed, "Emissive transition failed preparation");
    return status;
  };
  auto settle = [&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
    remixapi_PreparationStatisticsEXT status{};
    unsigned stable = 0;
    while (stable < 4) {
      status = frame();
      stable = tracingActive && !status.pending && !status.loading &&
          status.pipelinesRequested == status.pipelinesCompleted ? stable + 1 : 0;
      require(std::chrono::steady_clock::now() < deadline, "Emissive transition did not finish preparing");
      // Keep presenting while the worker compiles without flooding the queue
      // and validation layer with hundreds of thousands of loading frames.
      std::this_thread::sleep_for(std::chrono::milliseconds(stable ? 1 : 16));
    }
    return status;
  };
  const auto before = settle();
  std::fprintf(stderr, "Initial temporal configuration ready: %llu pipelines\n",
               static_cast<unsigned long long>(before.pipelinesCompleted));
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {.5f,.5f,.5f}; opaque.opacityConstant = 1; opaque.alphaTestType = 7;
  remixapi_MaterialInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO; info.pNext = &opaque; info.hash = 0xeeee01;
  info.emissiveColorConstant = {1,.5f,.1f}; info.emissiveIntensity = 4;
  ok(api.CreateMaterial(&info,&material));
  remixapi_HardcodedVertex vertices[3]{};
  const float positions[3][3]{{-1,-1,4},{1,-1,4},{0,1,4}};
  for (unsigned i = 0; i < 3; ++i) {
    std::copy_n(positions[i],3,vertices[i].position);
    vertices[i].normal[2] = -1; vertices[i].color = 0xffffffffu;
  }
  const uint32_t indices[]{0,1,2};
  remixapi_MeshInfoSurfaceTriangles surface{};
  surface.material = material; surface.vertices_values = vertices; surface.vertices_count = 3;
  surface.indices_values = indices; surface.indices_count = 3;
  remixapi_MeshInfo geometry{};
  geometry.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO; geometry.hash = 0xeeee02;
  geometry.surfaces_values = &surface; geometry.surfaces_count = 1;
  ok(api.CreateMesh(&geometry,&mesh));
  showEmitter = true;
  const auto after = settle();
  std::fprintf(stderr, "Emissive temporal configuration ready: %llu pipelines\n",
               static_cast<unsigned long long>(after.pipelinesCompleted));
  require(after.pipelinesRequested > before.pipelinesRequested, "Late emissive geometry did not prepare split lighting");
  remixapi_DenoisingStatisticsEXT temporal{};
  temporal.structSize = sizeof(temporal);
  ok(remixapi_GetDenoisingStatisticsEXT(&temporal));
  require(!std::strstr(temporal.unavailableReason, "initialization failed"),
          "Pending split pipelines permanently downgraded temporal lighting");
  require(temporal.activeTaa, "Late emissive transition lost temporal filtering");
  // Temporary visibility changes must not discard the prepared resource contract.
  // Warm each light-sampler variant once; those lazy pipelines are independent
  // of renderer/temporal-layout replacement.
  for (bool visible : {false,true}) {
    showEmitter = visible;
    settle();
  }
  const auto warmed = settle();
  for (bool visible : {false,true,false,true}) {
    showEmitter = visible;
    const auto status = settle();
    require(status.pipelinesRequested == warmed.pipelinesRequested, "Emissive visibility repeatedly rebuilt pipelines");
    remixapi_DenoisingStatisticsEXT current{};
    current.structSize = sizeof(current);
    ok(remixapi_GetDenoisingStatisticsEXT(&current));
    require(current.activeTaa && current.memoryBytes == temporal.memoryBytes,
            "Emissive visibility changed the temporal resource contract");
  }
  ok(api.DestroyMesh(mesh)); ok(api.DestroyMaterial(material));
  ok(api.Shutdown());
  std::puts("Late emissive lighting prepares asynchronously and survives visibility changes");
}
