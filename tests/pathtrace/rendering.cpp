#include "pathtrace/tlas_policy.h"
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <remix/remix_native_ext.h>
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
  void ok(remixapi_ErrorCode result) {
    if (result) {
      std::fprintf(stderr, "API failed: 0x%x\n", unsigned(result));
      std::exit(1);
    }
  }

} // namespace
int main(int argc, char **argv) {
  require(argc >= 2, "Supply scene output directory");
  remixapi_Interface api = {};
  remixapi_InitializeLibraryInfo init = {};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = 96;
  startup.windowHeight = 96;
#endif
  ok(api.Startup(&startup));
  // These fixtures measure the unexposed lighting/display contract.
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "off"));
  const bool denoiseFailure = argc > 2 && std::string(argv[2]) == "denoise-failure";
  if (denoiseFailure) {
    // Establish a working raw renderer before staging the failing replacement.
    ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", "off"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.taa", "0"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.sigma", "0"));
  }
  remixapi_BackendCapabilitiesEXT caps = {};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  require(caps.activeBackend == 0 && caps.requestedBackend == 0, "Raster must be default");
  require(api.SetConfigVariable("rtx.native.backend", "invalid") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid backend accepted");
  require(api.SetConfigVariable("rtx.native.pathtrace.samples", "17") ==
              REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid samples accepted");
  if (!(caps.supportedBackends & 2)) {
    require(api.SetConfigVariable("rtx.native.backend", "pathtrace") ==
                REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE,
            "Unsupported pathtrace selected");
    ok(remixapi_GetBackendCapabilitiesEXT(&caps));
    require(!caps.activeBackend && !caps.requestedBackend && caps.unavailableReason[0],
            "Unsupported fallback reason/state");
    ok(api.Present(nullptr));
    ok(api.Shutdown());
    std::printf("Unsupported-device fallback passed: %s\n", caps.unavailableReason);
    return 0;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  // The baseline lighting/composition checks decode the gamma 2.2 output.
  if (const char *primary = std::getenv("REMIX_TEST_PRIMARY_VISIBILITY"))
    ok(api.SetConfigVariable("rtx.native.pathtrace.primaryVisibility", primary));
  if (argc > 2 && std::string(argv[2]) == "init-failure") {
    // The injected pipeline failure arrives from asynchronous preparation.
    // Keep presenting the fallback until that request completes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
      ok(api.PumpEvents());
      ok(api.Present(nullptr));
      if (!remix::api::g_runtime.preparationStatus().pending)
        break;
      require(std::chrono::steady_clock::now() < deadline,
              "Injected tracing failure did not finish preparing");
      SDL_Delay(1);
    } while (true);
    ok(remixapi_GetBackendCapabilitiesEXT(&caps));
    require(!caps.activeBackend && caps.requestedBackend == 1 && caps.unavailableReason[0],
            "Tracing initialization failure must retain the preference while raster is active");
    ok(api.Present(nullptr));
    ok(api.Shutdown());
    return 0;
  }
  remixapi_LightInfoDistantEXT distant = {};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, 0, 1};
  remixapi_LightInfo light = {};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 10;
  light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
  remixapi_LightHandle sun;
  ok(api.CreateLight(&light, &sun));
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {1, 0, 0};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  remixapi_MaterialInfo material = {};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.pNext = &opaque;
  material.hash = 11;
  remixapi_MaterialHandle mat;
  ok(api.CreateMaterial(&material, &mat));
  remixapi_HardcodedVertex vertices[4] = {};
  const float positions[4][3] = {{-2, -2, .5f}, {2, -2, .5f}, {2, 2, .5f}, {-2, 2, .5f}};
  for (unsigned i = 0; i < 4; ++i) {
    std::copy_n(positions[i], 3, vertices[i].position);
    vertices[i].color = 0xffffffff;
  }
  uint32_t indices[] = {0, 1, 2, 0, 2, 3};
  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices;
  surface.vertices_count = 4;
  surface.indices_values = indices;
  surface.indices_count = 6;
  surface.material = mat;
  remixapi_MeshInfo mesh = {};
  mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  mesh.hash = 12;
  mesh.surfaces_values = &surface;
  mesh.surfaces_count = 1;
  remixapi_MeshHandle geometry;
  ok(api.CreateMesh(&mesh, &geometry));
  remixapi_InstanceIdentityEXT identity = {};
  identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
  identity.instanceId = 20;
  identity.rasterVisible = 1;
  remixapi_InstanceInfo instance = {};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.pNext = &identity;
  instance.mesh = geometry;
  instance.doubleSided = 1;
  for (int r = 0; r < 3; ++r)
    instance.transform.matrix[r][r] = 1;
  remixapi_CameraInfo camera = {};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  std::function<void()> extra = [] {};
  unsigned debugFrame = 0;
  auto submit = [&] {
    if (std::getenv("REMIX_TEST_FRAME_LOG"))
      std::fprintf(stderr, "GPU frame %u\n", ++debugFrame);
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    ok(api.DrawInstance(&instance));
    extra();
    ok(api.Present(nullptr));
  };
  auto frame = remix::test::preparedFrame(submit);
  auto statistics = [&] {
    remixapi_TracingStatisticsEXT s = {};
    s.structSize = sizeof(s);
    ok(remixapi_GetTracingStatisticsEXT(&s));
    return s;
  };
  for (int i = 0; i < 4; ++i)
    frame();
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  require(caps.activeBackend == 1, "Hardware pathtrace did not activate");
  if (argc > 2 && std::string(argv[2]) == "opaque-variants") {
    for (auto key : {"taa", "sharc", "sigma", "accumulate"})
      ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), "0"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", "off"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "0"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.primaryVisibility", "ray"));
    opaque.useDrawCallAlphaState = 1;
    ok(api.CreateMaterial(&material, &mat));
    remixapi_InstanceInfoBlendEXT alpha{};
    alpha.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    alpha.alphaTestEnabled = 1; alpha.alphaTestCompareOp = 7;
    alpha.alphaTestReferenceValue = 255; alpha.pNext = &identity;
    instance.pNext = &alpha;
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = .2f;
    instance.transform.matrix[0][3] = -.5f;
    uint32_t rightCompare = 2; // EQUAL 1: identical coverage, deliberately not proven opaque.
    extra = [&] {
      auto second = instance;
      auto secondAlpha = alpha;
      auto secondIdentity = identity; secondIdentity.instanceId = 21;
      secondAlpha.alphaTestCompareOp = rightCompare; secondAlpha.pNext = &secondIdentity;
      second.pNext = &secondAlpha; second.transform.matrix[0][3] = .5f;
      ok(api.DrawInstance(&second));
    };
    auto pixels = [&](const char *name) {
      const auto directory = std::filesystem::absolute(argv[1]);
      std::filesystem::create_directories(directory);
      const auto file = (directory / name).string();
      ok(remix::test::requestScreenshot(api, file.c_str(), frame));
      frame();
      frame();
      int w, h, n;
      auto bytes = stbi_load(file.c_str(), &w, &h, &n, 4);
      require(bytes != nullptr, "Missing opacity variant screenshot");
      std::vector<uint8_t> image(bytes, bytes + w*h*4); stbi_image_free(bytes);
      return image;
    };
    const auto mixed = pixels("mixed.png");
    rightCompare = 7;
    require(pixels("opaque.png") == mixed, "Shared-mesh opaque and alpha-tested visibility differ");
    alpha.alphaTestCompareOp = 2; rightCompare = 2;
    require(pixels("candidates.png") == mixed, "Candidate-only reference differs from hardware opacity");
    alpha.alphaTestCompareOp = 7; rightCompare = 0;
    const auto rejected = pixels("rejected.png");
    require(rejected != mixed, "Per-instance alpha rejection reused another instance's opaque BLAS");
    // Replacing the material keeps the geometry identity while changing coverage.
    opaque.useDrawCallAlphaState = 0; opaque.alphaTestType = 0;
    ok(api.CreateMaterial(&material, &mat));
    const auto replaced = pixels("replacement.png");
    require(replaced != rejected, "Material replacement retained stale opaque coverage");
    opaque.alphaTestType = 7; ok(api.CreateMaterial(&material, &mat));
    require(pixels("restored.png") == mixed, "Restoring opacity did not reuse the correct geometry variant");
    extra = [] {}; instance.transform.matrix[0][3] = 0;
    for (const char *primary : {"ray", "raster"}) {
    ok(api.SetConfigVariable("rtx.native.pathtrace.primaryVisibility", primary));
    instance.doubleSided = 1; instance.transform.matrix[0][0] = .2f;
    const auto twoSided = pixels("two-sided.png");
    instance.doubleSided = 0;
    require(pixels("front.png") == twoSided, "Single-sided front changed opaque traversal coverage");
    instance.transform.matrix[0][0] *= -1;
    require(pixels("back.png") != twoSided, "Mirrored single-sided back bypassed candidate rejection");
    instance.doubleSided = 1;
    require(pixels("mirrored.png") == twoSided, "Mirrored double-sided opaque geometry lost coverage");
    }
    // A single surface contains solid and rejected triangles. Start with the
    // cutout so hardware geometry partitioning must remap primitive identities.
    remixapi_HardcodedVertex atlasVertices[8] = {};
    for (unsigned i = 0; i < 8; ++i) {
      atlasVertices[i] = vertices[i % 4];
      atlasVertices[i].position[0] = positions[i % 4][0] * .2f + (i < 4 ? -.6f : .6f);
      atlasVertices[i].position[1] *= .4f;
      atlasVertices[i].color = i < 4 ? 0xffffffff : 0x00ffffff;
    }
    uint32_t atlasIndices[] = {4, 5, 6, 4, 6, 7, 0, 1, 2, 0, 2, 3};
    surface.vertices_values = atlasVertices; surface.vertices_count = 8;
    surface.indices_values = atlasIndices; surface.indices_count = 12;
    ok(api.CreateMesh(&mesh, &geometry)); instance.mesh = geometry;
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = 1;
    opaque.useDrawCallAlphaState = 1; ok(api.CreateMaterial(&material, &mat));
    alpha.textureAlphaOperation = 1; alpha.textureAlphaArg1Source = 2;
    for (const char *primary : {"ray", "raster"}) {
      ok(api.SetConfigVariable("rtx.native.pathtrace.primaryVisibility", primary));
      for (const char *debug : {"0", "3"}) {
        ok(api.SetConfigVariable("rtx.native.pathtrace.debugView", debug));
        alpha.alphaTestCompareOp = 4; alpha.alphaTestReferenceValue = 128;
        const auto partitioned = pixels("partitioned.png");
        alpha.alphaTestCompareOp = 5; alpha.alphaTestReferenceValue = 0;
        require(pixels("partition-reference.png") == partitioned,
                "Mixed opacity partition changed shading or original triangle identity");
        alpha.alphaTestCompareOp = 7;
        require(pixels("partition-all.png") != partitioned, "Changing mixed coverage retained stale BLAS flags");
      }
    }
    ok(api.Shutdown()); return 0;
  }
  if (argc > 2 && std::string(argv[2]) == "geometry-history") {
    ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "1"));
    frame();
    frame();
    const auto builds = statistics().blasBuilds;
    auto verifyEdit = [&](uint64_t expectedBuilds, const char *message) {
      ok(api.CreateMesh(&mesh, &geometry));
      frame();
      require(statistics().blasBuilds == expectedBuilds && statistics().accumulationCount == 1, message);
      frame();
      require(statistics().accumulationCount == 2, "Unchanged mesh reset accumulation");
      ok(api.CreateMesh(&mesh, &geometry));
      frame();
      require(statistics().accumulationCount == 3, "Identical mesh replacement reset accumulation");
    };
    require(statistics().accumulationCount >= 2, "Geometry history did not accumulate");
    vertices[0].texcoord[0] = .25f;
    verifyEdit(builds, "UV-only replacement rebuilt geometry or retained stale accumulation");
    vertices[0].color = 0xff00ffff;
    verifyEdit(builds, "Color-only replacement rebuilt geometry or retained stale accumulation");
    vertices[0].position[0] -= .01f;
    verifyEdit(builds + 1, "Position replacement did not rebuild geometry and invalidate accumulation");
    std::swap(indices[1], indices[2]);
    verifyEdit(builds + 2, "Index replacement did not rebuild geometry and invalidate accumulation");
    ok(api.Shutdown());
    std::puts("Geometry edits, shading edits, exact cache reuse and accumulation passed");
    return 0;
  }
  if (argc > 2 && std::string(argv[2]) == "mesh-update") {
    remixapi_MeshInfoUpdateEXT update{};
    update.sType = REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT; update.enabled = 1; mesh.pNext = &update;
    instance.pNext = nullptr; // The hint supplies storage reuse, not actor identity.
    for (auto &v : vertices) { v.position[0] *= .3f; v.position[1] *= .3f; }
    ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "1"));
    ok(api.CreateMesh(&mesh,&geometry)); frame(); frame();
    auto before = statistics();
    auto edit = [&] { ok(api.CreateMesh(&mesh,&geometry)); frame(); };
    for (unsigned i = 0; i < 60; ++i) { vertices[0].position[0] -= .001f; edit(); }
    auto refitted = statistics();
    require(refitted.blasBuilds == before.blasBuilds && refitted.blasRefits == before.blasRefits + 60,
            "Compatible finite vertices did not refit");
    require(refitted.accumulationCount == 1, "Refitted mesh retained stale history");
    frame(); require(statistics().accumulationCount == 2, "Held update mesh did not accumulate");
    require(statistics().blasRefits == refitted.blasRefits, "Held mesh refit again");
    ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "0"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.taa", "0"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", "off"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "0"));
    const auto directory = std::filesystem::absolute(std::filesystem::path(argv[1]));
    std::filesystem::create_directories(directory);
    auto pixels = [&](const char *name) {
      const auto file = (directory/name).string();
      ok(remix::test::requestScreenshot(api, file.c_str(), frame));
      frame();
      frame();
      int w, h, n; auto *data = stbi_load(file.c_str(), &w, &h, &n, 4);
      require(data, "Mesh-update screenshot unavailable");
      std::vector<unsigned char> result(data, data+w*h*4); stbi_image_free(data);
      require(std::any_of(result.begin(),result.end(),[](auto c) { return c > 0 && c < 255; }),
              "Mesh-update image has no shaded geometry");
      return result;
    };
    // These settings prepare a replacement tracer with its own geometry: its
    // first frame builds the mesh, then refit that up to the limit again.
    frame();
    for (unsigned i = 0; i < 60; ++i) { vertices[0].position[0] -= .001f; edit(); }
    refitted = statistics();
    const auto refitImage = pixels("refitted.png");
    // UV-only replacement preserves this white-texture image while the refit
    // limit forces a fresh build from precisely the same positions and indices.
    vertices[0].texcoord[0] += .1f; edit();
    require(statistics().blasBuilds == refitted.blasBuilds + 1, "Refit limit did not rebuild");
    require(pixels("rebuilt.png") == refitImage, "Refitted and rebuilt geometry images disagree");
    before = statistics(); vertices[0].position[0] -= 10; edit();
    require(statistics().blasBuilds == before.blasBuilds + 1, "Large displacement did not rebuild");
    before = statistics(); std::swap(indices[1],indices[2]); edit();
    require(statistics().blasBuilds == before.blasBuilds + 1 && statistics().blasRefits == before.blasRefits,
            "Index edit used an incompatible refit");
    before = statistics(); surface.indices_count = 3; edit();
    require(statistics().blasBuilds == before.blasBuilds + 1, "Primitive count edit did not rebuild");
    before = statistics(); ok(api.DestroyMesh(geometry)); edit();
    require(statistics().blasBuilds == before.blasBuilds + 1, "Destroy/recreate reused stale update storage");
    // Two immutable replacements can be queued in the same frame. The second
    // must get its own BLAS while the first is referenced by this command.
    before = statistics(); ok(api.DrawInstance(&instance)); vertices[0].position[0] += .001f; edit();
    require(statistics().blasBuilds == before.blasBuilds + 1, "Queued mesh versions aliased one mutable BLAS");
    ok(api.Shutdown());
    std::puts("Mesh updates: refits, exact reuse, history, periodic/displacement rebuilds, topology and lifetime isolation passed");
    return 0;
  }
  if (argc > 2 && std::string(argv[2]) == "mesh-refresh") {
    // remixapi_MeshInfoDeformEXT: a mesh
    // created normally, then refreshed under the same hash (FaceGen morphs,
    // particles). The first refresh starts a lineage (one build without
    // compaction); later ones refit that BLAS in place, so the dynamic TLAS
    // refits instead of rebuilding for a new BLAS address.
    ok(api.SetConfigVariable("rtx.native.meshes.refreshInPlace", "1"));
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    remixapi_MeshInfoDeformEXT refresh{};
    refresh.sType = REMIXAPI_STRUCT_TYPE_MESH_DEFORM_EXT;
    for (auto &v : vertices) { v.position[0] *= .3f; v.position[1] *= .3f; }
    auto measure = [&](const char *what, remixapi_MeshHandle &handle) {
      mesh.pNext = nullptr;
      ok(api.CreateMesh(&mesh, &handle));
      for (int i = 0; i < 8; ++i) frame(); // Settle builds and compaction.
      mesh.pNext = &refresh;
      auto edit = [&] { ok(api.CreateMesh(&mesh, &handle)); frame(); };
      const auto before = statistics();
      vertices[0].position[0] -= .001f; edit();
      const auto started = statistics();
      require(started.blasBuilds == before.blasBuilds + 1, "First refresh did not start one lineage build");
      for (unsigned i = 0; i < 30; ++i) { vertices[0].position[0] -= .001f; edit(); }
      for (int i = 0; i < 4; ++i) frame();
      const auto refreshed = statistics();
      std::printf("%s: 30 refreshes -> BLAS builds +%llu, refits +%llu, compactions +%llu, dynamic TLAS builds +%llu, "
                  "refits +%llu\n", what,
                  (unsigned long long)(refreshed.blasBuilds - started.blasBuilds),
                  (unsigned long long)(refreshed.blasRefits - started.blasRefits),
                  (unsigned long long)(refreshed.blasCompactions - started.blasCompactions),
                  (unsigned long long)(refreshed.dynamicTlasBuilds - started.dynamicTlasBuilds),
                  (unsigned long long)(refreshed.dynamicTlasRefits - started.dynamicTlasRefits));
      require(refreshed.blasBuilds == started.blasBuilds && refreshed.blasRefits == started.blasRefits + 30 &&
                  refreshed.blasCompactions == started.blasCompactions,
              "Refreshed geometry was rebuilt or compacted instead of refitted");
      require(refreshed.dynamicTlasBuilds == started.dynamicTlasBuilds &&
                  refreshed.dynamicTlasRefits >= started.dynamicTlasRefits + 30,
              "Refreshed geometry rebuilt the dynamic TLAS");
      // Identical content is not a refresh; a changed surface count is ignored.
      edit();
      require(statistics().blasRefits == refreshed.blasRefits, "Identical refresh refit");
      mesh.surfaces_count = 2;
      remixapi_MeshInfoSurfaceTriangles pair[2] = {surface, surface};
      mesh.surfaces_values = pair;
      vertices[0].position[0] -= .001f; edit();
      require(statistics().blasBuilds == refreshed.blasBuilds && statistics().blasRefits == refreshed.blasRefits,
              "Surface-count change was not ignored");
      mesh.surfaces_count = 1;
      mesh.surfaces_values = &surface;
    };
    remixapi_MeshHandle refreshed;
    mesh.hash = 40;
    instance.mesh = reinterpret_cast<remixapi_MeshHandle>(uintptr_t(mesh.hash));
    measure("Rigid refresh", refreshed);
    // Skinned (FaceGen heads): the lineage transfers the instance's own BLAS.
    float weights[] = {1, 1, 1, 1};
    uint32_t bones[] = {0, 0, 0, 0};
    surface.skinning_hasvalue = 1;
    surface.skinning_value = {1, weights, 4, bones, 4};
    remixapi_Transform bone = {};
    for (int i = 0; i < 3; ++i)
      bone.matrix[i][i] = 1;
    remixapi_InstanceInfoBoneTransformsEXT skin = {};
    skin.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
    skin.pNext = &identity;
    skin.boneTransforms_count = 1;
    skin.boneTransforms_values = &bone;
    instance.pNext = &skin;
    mesh.hash = 41;
    instance.mesh = reinterpret_cast<remixapi_MeshHandle>(uintptr_t(mesh.hash));
    measure("Skinned refresh", refreshed);
    ok(api.Shutdown());
    std::puts("Mesh refresh: in-place refits for rigid and skinned meshes, no compaction or TLAS rebuild passed");
    return 0;
  }
  remixapi_DenoisingCapabilitiesEXT denoiseCaps{};
  denoiseCaps.structSize = sizeof(denoiseCaps);
  ok(remixapi_GetDenoisingCapabilitiesEXT(&denoiseCaps));
  auto denoiseStats = [&] {
    remixapi_DenoisingStatisticsEXT s{};
    s.structSize = sizeof(s);
    ok(remixapi_GetDenoisingStatisticsEXT(&s));
    return s;
  };
  auto initialDenoising = denoiseStats();
  if (denoiseFailure && denoiseCaps.taaSupported) {
    const auto before = statistics();
    ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", "reblur-sh"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.taa", "1"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    do {
      submit();
      require(std::chrono::steady_clock::now() < deadline, "Temporal failure did not finish preparing");
    } while (remix::api::g_runtime.preparationStatus().pending);
    const auto failed = denoiseStats();
    require(!remix::api::g_runtime.preparationStatus().failure.empty() &&
                !failed.activeDenoiser && !failed.activeTaa,
            "Temporal allocation failure did not retain the active raw renderer");
    submit();
    submit();
    require(statistics().sampleSequence > before.sampleSequence,
            "Raw tracing stopped after temporal preparation failure");
    ok(api.Shutdown());
    return 0;
  }
  require(initialDenoising.requestedDenoiser == 1 && initialDenoising.requestedTaa == 1,
          "Temporal defaults not requested");
  require(initialDenoising.activeDenoiser == denoiseCaps.reblurShSupported &&
              initialDenoising.activeTaa == denoiseCaps.taaSupported,
          "Supported temporal defaults not active");
  if (initialDenoising.activeDenoiser || initialDenoising.activeTaa) {
    for (int i = 0; i < 80 && (denoiseStats().redrawRequested || statistics().redrawRequested); ++i)
      frame();
    require(!denoiseStats().redrawRequested && !statistics().redrawRequested, "Unchanged world never converged");
    auto converged = denoiseStats();
    frame();
    frame();
    require(denoiseStats().renderedFrames == converged.renderedFrames,
            "UI-only redraw retraced converged world");
    camera.view[3][0] = .01f;
    frame();
    require(denoiseStats().renderedFrames == converged.renderedFrames + 1 && denoiseStats().redrawRequested,
            "Camera motion did not resume temporal redraws");
    camera.view[3][0] = 0;
    frame();
  }
  auto warm = statistics();
  require(warm.blasBuilds == 1, "Static BLAS not cached");
  for (int i = 0; i < 4; ++i)
    frame();
  auto stable = statistics();
  require(stable.blasBuilds == warm.blasBuilds && stable.staticTlasBuilds == warm.staticTlasBuilds &&
              stable.dynamicTlasRefits == warm.dynamicTlasRefits,
          "Warm scene acceleration work");
  camera.view[3][0] = .1f;
  frame();
  require(denoiseStats().historyResets == initialDenoising.historyResets, "Camera motion reset denoiser");
  require(statistics().staticTlasBuilds == warm.staticTlasBuilds, "Camera motion rebuilt acceleration");
  ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "1"));
  frame();
  frame();
  require(!denoiseStats().activeDenoiser && !denoiseStats().activeTaa && denoiseStats().requestedDenoiser &&
              denoiseStats().requestedTaa,
          "Reference accumulation failed to bypass temporal filters");
  auto accumulated = statistics();
  require(accumulated.accumulationCount >= 2, "Accumulation not advancing");
  ok(api.CreateMesh(&mesh, &geometry));
  ok(api.CreateMaterial(&material, &mat));
  frame();
  require(statistics().accumulationCount == accumulated.accumulationCount + 1,
          "Identical resubmission reset accumulation");
  const auto path = std::filesystem::absolute(std::filesystem::path(argv[1]));
  std::filesystem::create_directories(path);
  const auto red = (path / "diffuse-red.png").string();
  ok(remix::test::requestScreenshot(api, red.c_str(), frame));
  frame();
  frame();
  int width, height, channels;
  auto pixels = stbi_load(red.c_str(), &width, &height, &channels, 4);
  require(pixels, "Read traced screenshot");
  const auto *center = pixels + (height / 2 * width + width / 2) * 4;
  std::printf("Traced red center: %u %u %u; BLAS %llu compactions %llu\n", center[0], center[1], center[2],
              (unsigned long long)stable.blasBuilds, (unsigned long long)stable.blasCompactions);
  require(center[0] > 240 && center[1] < 5 && center[2] < 5, "Diffuse light / color / composition pixel");
  stbi_image_free(pixels);
  opaque.albedoConstant = {0, 1, 0};
  ok(api.CreateMaterial(&material, &mat));
  frame();
  require(statistics().accumulationCount == 1 && statistics().blasBuilds == stable.blasBuilds,
          "Material change acceleration/invalidation");
  identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
  frame();
  warm = statistics();
  instance.transform.matrix[0][3] += .01f;
  frame();
  stable = statistics();
  require(stable.blasBuilds == warm.blasBuilds && stable.dynamicTlasRefits == warm.dynamicTlasRefits + 1,
          "Rigid animation refit");
  frame();
  require(statistics().dynamicTlasRefits == stable.dynamicTlasRefits, "Unchanged dynamic TLAS work");
  ok(api.SetConfigVariable("rtx.native.pathtrace.forceRebuild", "1"));
  frame();
  require(statistics().dynamicRebuildReason == REMIXAPI_REBUILD_FORCED_EXT, "Forced rebuild mode");
  ok(api.SetConfigVariable("rtx.native.pathtrace.forceRebuild", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "0"));
  camera.view[3][0] = 0;
  instance.transform.matrix[0][3] = 0;
  opaque.albedoConstant = {1, 1, 1};
  opaque.useDrawCallAlphaState = 1;
  ok(api.CreateMaterial(&material, &mat));
  auto capture = [&](const char *name) {
    auto file = (path / name).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, n;
    auto bytes = stbi_load(file.c_str(), &w, &h, &n, 4);
    require(bytes, "Scene screenshot");
    std::array<unsigned, 3> color = {bytes[(h / 2 * w + w / 2) * 4], bytes[(h / 2 * w + w / 2) * 4 + 1],
                                     bytes[(h / 2 * w + w / 2) * 4 + 2]};
    stbi_image_free(bytes);
    return color;
  };
  if (std::getenv("REMIX_TEST_TAA_STABILITY") || std::getenv("REMIX_TEST_FSR_STABILITY")) {
    const bool fsr = std::getenv("REMIX_TEST_FSR_STABILITY") != nullptr;
    // A stationary slanted silhouette has different coverage at each Halton
    // phase. Its integrated edge position must stay fixed in display pixels.
    // Moving the orthographic camera only along Z prevents convergence caching
    // without changing the projected silhouette or its deterministic lighting.
    require(denoiseCaps.taaSupported, "TAA stability test requires temporal support");
    const bool animatedAtlas = std::getenv("REMIX_TEST_TAA_ATLAS") != nullptr;
    uint8_t atlasPixels[] = {255, 255, 255, 255, 0, 0, 0, 255};
    remixapi_TextureInfo atlas{};
    atlas.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    atlas.hash = 40;
    atlas.width = 2;
    atlas.height = 1;
    atlas.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    atlas.data = atlasPixels;
    atlas.dataSize = sizeof(atlasPixels);
    remixapi_TextureHandle atlasHandle{};
    if (animatedAtlas) {
      ok(api.CreateTexture(&atlas, &atlasHandle));
#if defined(_WIN32)
      material.albedoTexture = L"0x28";
#else
      material.albedoTexture = "0x28";
#endif
      ok(api.CreateMaterial(&material, &mat));
      for (auto &vertex : vertices) {
        vertex.texcoord[0] = .25f;
        vertex.texcoord[1] = .5f;
      }
    }
    if (fsr) {
      // Preserve this fixture's projected geometry while using perspective depth.
      camera.projection[0][0] = camera.projection[1][1] = .5f;
      camera.projection[2][2] = 100.f / 99.95f;
      camera.projection[2][3] = 1;
      camera.projection[3][2] = -5.f / 99.95f;
      camera.projection[3][3] = 0;
      ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "fsr31"));
      ok(api.SetConfigVariable("rtx.native.pathtrace.fsrQuality", "quality"));
    }
    vertices[1].position[0] = -.45f;
    vertices[2].position[0] = .55f;
    ok(api.CreateMesh(&mesh, &geometry));
    instance.mesh = geometry;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    ok(api.SetConfigVariable("rtx.native.vsync", "0"));
    unsigned tick = 0;
    extra = [&] {
      ++tick;
      if (fsr) {
        // Change scene metadata to prevent frame caching without moving the edge.
        distant.direction.x = (tick % 2) ? .0001f : 0.f;
        ok(api.UpdateLightDefinition(sun, &light));
      } else camera.view[3][2] = (tick % 2) ? .0001f : 0.f;
      ok(api.SetupCamera(&camera));
      if (animatedAtlas) {
        // Only the unseen atlas tile changes. Every subsequent draw resolves
        // the updated material, while the visible white edge remains fixed.
        atlasPixels[4] = uint8_t(tick);
        ok(api.CreateTexture(&atlas, &atlasHandle));
        ok(api.CreateMaterial(&material, &mat));
      }
    };
    bool passed = true;
    for (const char *primary : {"raster", "ray"}) {
      ok(api.SetConfigVariable("rtx.native.pathtrace.primaryVisibility", primary));
      for (unsigned mode = 0; mode < 4; ++mode) {
        const bool taa = (mode & 1) != 0, reblur = (mode & 2) != 0;
        if (fsr && !taa) continue;
        if (reblur && !denoiseCaps.reblurShSupported)
          continue;
        ok(api.SetConfigVariable("rtx.native.pathtrace.taa", taa ? "1" : "0"));
        ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", reblur ? "reblur-sh" : "off"));
        for (unsigned i = 0; i < (fsr ? 64u : 16u); ++i)
          frame();
        double lo = 1e10, hi = -1e10;
        unsigned coveragePixels = 0;
        for (unsigned sample = 0; sample < 16; ++sample) {
          // capture() presents twice; shift parity halfway through to cover all
          // sixteen jitter phases, rather than repeating just the even phases.
          if (sample == 8)
            frame();
          const auto name = std::string("taa-edge-") + primary + "-" + std::to_string(mode) + "-" +
                            std::to_string(sample) + ".png";
          capture(name.c_str());
          int w, h, n;
          auto bytes = stbi_load((path / name).string().c_str(), &w, &h, &n, 4);
          require(bytes, "Read stationary edge frame");
          double edge = 0;
          unsigned rows = 0;
          auto linear = [](unsigned char byte) {
            const float value = byte / 255.f;
            return std::pow(value, 2.2f);
          };
          for (int y = h / 4; y < 3 * h / 4; ++y) {
            ++rows;
            const float black = linear(bytes[(y * w + 3 * w / 4) * 4]);
            const float white = linear(bytes[(y * w + w / 4) * 4]);
            require(white - black > .8f, "Stationary edge lacks expected contrast");
            for (int x = w / 4; x < 3 * w / 4; ++x) {
              const float value = (linear(bytes[(y * w + x) * 4]) - black) / (white - black);
              edge += value;
              coveragePixels += value > .05f && value < .95f;
            }
          }
          stbi_image_free(bytes);
          edge /= rows;
          lo = std::min(lo, edge);
          hi = std::max(hi, edge);
        }
        const auto denoising = denoiseStats();
        require(denoising.activeTaa == (taa && !fsr) && denoising.activeDenoiser == reblur,
                "Stationary edge did not exercise requested temporal modes");
        std::printf("Stationary %s edge, TAA %u NRD %u: %.4f px travel, %u coverage pixels\n",
                    primary, taa, reblur, hi - lo, coveragePixels);
        passed &= hi - lo < (taa ? .08 : .001);
        if (taa)
          passed &= coveragePixels > 16 * 24;
      }
    }
    ok(api.Shutdown());
    require(passed, "Stationary silhouette follows camera jitter or lacks antialiased coverage");
    return 0;
  }
  instance.doubleSided = 0;
  auto facing = capture("single-sided.png");
  require(facing[0] > 240, "Ray-query front face disagrees with raster clockwise fronts");
  instance.doubleSided = 1;
  // Subpixel motion of a two-pixel-wide surface must stay visible without a
  // broad history trail. Retain these frames for visual edge inspection.
  instance.transform.matrix[0][0] = .01f;
  for (unsigned sample = 0; sample < 4; ++sample) {
    instance.transform.matrix[0][3] = float(sample) * .005f;
    for (unsigned i = 0; i < 8; ++i)
      frame();
    auto name = "thin-motion-" + std::to_string(sample) + ".png";
    capture(name.c_str());
    int w, h, n;
    auto bytes = stbi_load((path / name).string().c_str(), &w, &h, &n, 4);
    require(bytes, "Read thin surface frame");
    unsigned litPixels = 0;
    for (int x = 0; x < w; ++x)
      litPixels += bytes[(h / 2 * w + x) * 4] > 180;
    stbi_image_free(bytes);
    require(litPixels >= 1 && litPixels <= 8, "Thin geometry vanished or retained a broad history trail");
  }
  instance.transform.matrix[0][0] = 1;
  instance.transform.matrix[0][3] = 0;
  // An offscreen, raster-culled rigid instance shadows the visible receiver.
  auto blocker = instance;
  remixapi_InstanceIdentityEXT blockerId = identity;
  blockerId.instanceId = 21;
  blockerId.rasterVisible = 0;
  blocker.pNext = &blockerId;
  for (int r = 0; r < 3; ++r)
    blocker.transform.matrix[r][r] = .25f;
  blocker.transform.matrix[0][3] = 2.5f;
  blocker.transform.matrix[2][3] = -.125f;
  distant.direction = {-.9805807f, 0, .1961161f};
  ok(api.UpdateLightDefinition(sun, &light));
  auto lit = capture("unoccluded.png");
  require(lit[0] > 80, "Unoccluded distant light");
  extra = [&] { ok(api.DrawInstance(&blocker)); };
  auto shadowed = capture("offscreen-shadow.png");
  require(shadowed[0] < 5, "Offscreen shadow missing");
  remixapi_InstanceInfoBlendEXT blend = {};
  blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  blend.pNext = &blockerId;
  blocker.pNext = &blend;
  blend.alphaTestEnabled = 1;
  blend.alphaTestCompareOp = 4;
  blend.alphaTestReferenceValue = 128;
  blend.textureAlphaOperation = 1;
  blend.textureAlphaArg1Source = 3;
  blend.tFactor = 0x00ffffff;
  auto cutout = capture("cutout-shadow.png");
  require(cutout[0] > 80, "Cutout shadow ray rejected transparency");
  // The same cutout handling applies to primary visibility.
  blocker.transform = instance.transform;
  blocker.transform.matrix[2][3] = -.3f;
  auto primaryCutout = capture("cutout-primary.png");
  require(primaryCutout[0] > 80, "Primary cutout handling");
  // Texture alpha replacement changes coverage and may select a different
  // cached opacity variant; position/index topology still stays shared.
  uint8_t texel[] = {0, 255, 0, 0};
  remixapi_TextureInfo texture = {};
  texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  texture.hash = 40;
  texture.width = texture.height = 1;
  texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  texture.data = texel;
  texture.dataSize = 4;
  remixapi_TextureHandle textureHandle;
  ok(api.CreateTexture(&texture, &textureHandle));
  material.hash = 41;
#if defined(_WIN32)
  material.albedoTexture = L"0x28";
#else
  material.albedoTexture = "0x28";
#endif
  remixapi_MaterialHandle textured;
  ok(api.CreateMaterial(&material, &textured));
  surface.material = textured;
  mesh.hash = 42;
  remixapi_MeshHandle texturedMesh;
  ok(api.CreateMesh(&mesh, &texturedMesh));
  surface.material = mat;
  mesh.hash = 12;
  blocker.mesh = texturedMesh;
  blend.textureAlphaArg1Source = 1;
  blend.tFactor = 0xffffffff;
  auto textureHole = capture("texture-cutout.png");
  require(textureHole[0] > 80 && textureHole[1] > 80, "Texture alpha hole missing");
  auto textureBuilds = statistics().blasBuilds;
  texel[3] = 255;
  ok(api.CreateTexture(&texture, &textureHandle));
  ok(api.CreateMaterial(&material, &textured));
  auto textureSolid = capture("texture-alpha-replaced.png");
  require(textureSolid[0] < 5 && textureSolid[1] > 80 && statistics().blasBuilds <= textureBuilds + 1,
          "Texture alpha replacement retained stale coverage or rebuilt unrelated geometry");
  material.hash = 11;
  material.albedoTexture = nullptr;
  blocker.mesh = geometry;
  blend.textureAlphaArg1Source = 3;
  ok(api.DestroyMesh(texturedMesh));
  ok(api.DestroyMaterial(textured));
  ok(api.DestroyTexture(textureHandle));
  distant.direction = {0, 0, 1};
  ok(api.UpdateLightDefinition(sun, &light));
  blend.alphaTestEnabled = 0;
  blend.alphaBlendEnabled = 1;
  blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
  blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
  blend.textureColorOperation = 3;
  blend.textureColorArg1Source = 1;
  blend.textureColorArg2Source = 3;
  blend.tFactor = 0x0000ff00;
  auto uncovered = capture("zero-alpha-transmission.png");
  require(uncovered[0] > 240 && uncovered[1] > 240 && uncovered[2] > 240,
          "Zero-coverage primary/shadow transmission was tinted by source RGB");
  // Raster gameplay feedback shares primary world depth.
  blockerId.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
  blockerId.rasterVisible = 1;
  blend.alphaBlendEnabled = 0;
  blend.tFactor = 0xff0000ff;
  blocker.transform.matrix[2][3] = .3f;
  auto behind = capture("overlay-behind.png");
  require(behind[0] > 240 && behind[1] > 240, "Raster overlay ignored world depth");
  blocker.transform.matrix[2][3] = -.3f;
  auto front = capture("overlay-front.png");
  require(front[2] > 240 && front[0] < 5, "Raster foreground overlay missing");
  extra = [] {};
  auto builds = statistics().blasBuilds;
  camera.view[3][0] = -1024;
  instance.transform.matrix[0][3] = 1024;
  auto large = capture("floating-origin.png");
  require(large[0] > 240 && statistics().blasBuilds == builds,
          "Floating-origin geometry rebuilt or incorrect");
  camera.view[3][0] = 0;
  instance.transform.matrix[0][3] = 0;
  ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "1"));
  frame();
  frame();
  auto countBeforeUi = statistics().accumulationCount;
  ok(api.SetScreenTint(0, 0, 0, .1f));
  frame();
  require(statistics().accumulationCount == countBeforeUi + 1, "UI-only change reset accumulation");
  ok(api.SetScreenTint(0, 0, 0, 0));
  // A red side wall transfers diffuse light to a receiver with zero direct illumination.
  ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "0"));
  blockerId.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
  blockerId.rasterVisible = 0;
  blocker.transform = {};
  blocker.transform.matrix[0][2] = 1;
  blocker.transform.matrix[1][1] = 1;
  blocker.transform.matrix[2][0] = -1;
  blocker.transform.matrix[0][3] = .3f;
  blend.tFactor = 0xffff0000;
  distant.direction = {1, 0, 0};
  ok(api.UpdateLightDefinition(sun, &light));
  extra = [&] { ok(api.DrawInstance(&blocker)); };
  auto noBounce = capture("indirect-disabled.png");
  require(noBounce[0] < 5, "Sky supplied unintended environment light");
  ok(api.SetConfigVariable("rtx.native.pathtrace.samples", "16"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "1"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "1"));
  for (int i = 0; i < 32; ++i)
    frame();
  auto transferred = capture("diffuse-color-transfer.png");
  require(transferred[0] > 30 && transferred[1] < 5 && transferred[2] < 5, "Diffuse color transfer missing");
  if (std::getenv("REMIX_TEST_DENOISE_QUALITY") && denoiseCaps.reblurShSupported) {
    auto readRegion = [&](const std::string &file) {
      int w, h, n;
      auto bytes = stbi_load(file.c_str(), &w, &h, &n, 4);
      require(bytes, "Read quality frame");
      std::vector<double> values;
      for (int y = h / 2 - 12; y < h / 2 + 12; ++y)
        for (int x = w / 2 - 12; x < w / 2 + 12; ++x) {
          const double c = double(bytes[(y * w + x) * 4]) / 255;
          values.push_back(std::pow(c, 2.2));
        }
      stbi_image_free(bytes);
      return values;
    };
    const auto reference = readRegion((path / "diffuse-color-transfer.png").string());
    struct Quality {
      double error = 0, variance = 0;
    };
    Quality results[4];
    ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "0"));
    ok(api.SetConfigVariable("rtx.native.pathtrace.samples", "1"));
    for (unsigned mode = 0; mode < 4; ++mode) {
      ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", mode >= 2 ? "reblur-sh" : "off"));
      ok(api.SetConfigVariable("rtx.native.pathtrace.taa", mode % 2 ? "1" : "0"));
      for (unsigned i = 0; i < 80; ++i) {
        camera.view[3][0] = float(i % 2) * .0001f;
        frame();
      }
      std::vector<double> sum(reference.size()), squares(reference.size());
      for (unsigned sample = 0; sample < 16; ++sample) {
        camera.view[3][0] = float(sample % 2) * .0001f;
        auto name = "quality-" + std::to_string(mode) + "-" + std::to_string(sample) + ".png";
        capture(name.c_str());
        auto values = readRegion((path / name).string());
        for (size_t i = 0; i < values.size(); ++i) {
          double d = values[i] - reference[i];
          results[mode].error += d * d;
          sum[i] += values[i];
          squares[i] += values[i] * values[i];
        }
      }
      results[mode].error /= 16 * reference.size();
      for (size_t i = 0; i < sum.size(); ++i)
        results[mode].variance += squares[i] / 16 - std::pow(sum[i] / 16, 2);
      results[mode].variance /= sum.size();
      std::printf("Quality mode %u: MSE %.9f variance %.9f\n", mode, results[mode].error,
                  results[mode].variance);
    }
    require(results[2].error < results[0].error && results[2].variance < results[0].variance,
            "REBLUR SH did not improve diffuse lighting error and temporal variance");
    require(results[3].error < results[0].error && results[3].variance < results[0].variance,
            "REBLUR SH plus TAA did not improve diffuse error and variance");
    camera.view[3][0] = 0;
    // Removing an offscreen source of indirect light must update local lighting
    // without a global reset. Keep a motion sequence for visual ghosting review.
    auto beforeRemoval = denoiseStats();
    extra = [] {};
    for (unsigned i = 0; i <= 32; ++i) {
      camera.view[3][0] = .1f * float(i) / 32;
      if (i == 0 || (i & (i - 1)) == 0) {
        auto name = "motion-light-removed-" + std::to_string(i) + ".png";
        auto color = capture(name.c_str());
        if (i == 32)
          require(color[0] < 12, "Offscreen lighting remained stale after convergence");
      } else
        frame();
    }
    require(denoiseStats().historyResets == beforeRemoval.historyResets,
            "Offscreen geometry/camera motion reset all history");
    camera.view[3][0] = 0;
  }

  extra = [] {};
  distant.direction = {0, 0, 1};
  ok(api.UpdateLightDefinition(sun, &light));
  ok(api.SetConfigVariable("rtx.native.pathtrace.samples", "1"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.accumulate", "0"));
  // Vertex deformation updates a private BLAS and refreshes its TLAS bounds.
  float weights[] = {1, 1, 1, 1};
  uint32_t bones[] = {0, 0, 0, 0};
  surface.skinning_hasvalue = 1;
  surface.skinning_value = {1, weights, 4, bones, 4};
  mesh.hash = 30;
  remixapi_MeshHandle skinned;
  ok(api.CreateMesh(&mesh, &skinned));
  instance.mesh = skinned;
  remixapi_Transform bone = {};
  for (int i = 0; i < 3; ++i)
    bone.matrix[i][i] = 1;
  remixapi_InstanceInfoBoneTransformsEXT skin = {};
  skin.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
  skin.pNext = &identity;
  skin.boneTransforms_count = 1;
  skin.boneTransforms_values = &bone;
  instance.pNext = &skin;
  frame();
  auto beforeSkin = statistics();
  bone.matrix[0][3] = .2f;
  frame();
  auto afterSkin = statistics();
  require(afterSkin.blasBuilds == beforeSkin.blasBuilds &&
              afterSkin.blasRefits == beforeSkin.blasRefits + 1 &&
              afterSkin.dynamicTlasRefits == beforeSkin.dynamicTlasRefits + 1,
          "Deformation BLAS/TLAS refit");
  frame();
  require(statistics().blasRefits == afterSkin.blasRefits, "Identical deformation refit");
  instance.mesh = geometry;
  instance.pNext = &identity;
  ok(api.DestroyMesh(skinned));
  // Legacy frame-local IDs coexist with every legal persistent ID.
  identity.instanceId = UINT64_MAX;
  auto legacyInstance = instance;
  legacyInstance.pNext = nullptr;
  legacyInstance.transform.matrix[0][3] = 10;
  extra = [&] { ok(api.DrawInstance(&legacyInstance)); };
  frame();
  frame();
  extra = [] {};
  ok(api.DrawInstance(&instance));
  require(api.DrawInstance(&instance) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Duplicate persistent identity accepted");
  ok(api.Present(nullptr));
  identity.instanceId = 20;
  frame();
  // Replacement preserves queued geometry; only its changed BLAS builds next frame.
  auto beforeReplacement = statistics();
  ok(api.SetupCamera(&camera));
  ok(api.DrawLightInstance(sun));
  ok(api.DrawInstance(&instance));
  mesh.hash = 12;
  surface.skinning_hasvalue = 0;
  vertices[0].position[0] -= .1f;
  ok(api.CreateMesh(&mesh, &geometry));
  ok(api.Present(nullptr));
  require(statistics().blasBuilds == beforeReplacement.blasBuilds, "Queued mesh version replaced early");
  frame();
  require(statistics().blasBuilds == beforeReplacement.blasBuilds + 1, "Mesh edit rebuilt unrelated BLASes");
  frame();
  frame();
  builds = statistics().blasBuilds;
  vertices[0].texcoord[0] = .25f;
  ok(api.CreateMesh(&mesh, &geometry));
  frame();
  require(statistics().blasBuilds == builds, "UV-only replacement rebuilt acceleration geometry");
  // The last selected distant light may be black, and destroyed handles fail.
  light.radiance = {0, 0, 0};
  ok(api.UpdateLightDefinition(sun, &light));
  auto black = capture("zero-light.png");
  require(black[0] < 5 && black[1] < 5 && black[2] < 5,
          "Black distant light did not replace previous selection");
  light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
  ok(api.UpdateLightDefinition(sun, &light));
  std::puts("Diffuse color transfer and deformation BLAS/TLAS updates passed");
  std::puts("Offscreen shadows, primary/shadow cutouts, tinted transmission, floating origin and raster "
            "depth passed");
  if (denoiseCaps.reblurShSupported) {
    remixapi_CameraTemporalEXT temporal{};
    temporal.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
    temporal.worldEpoch = 1;
    temporal.historyResetEpoch = 1;
    camera.pNext = &temporal;
    frame();
    auto beforeReset = denoiseStats();
    ++temporal.historyResetEpoch;
    frame();
    require(denoiseStats().historyResets == beforeReset.historyResets + 1,
            "Camera cut/rebase epoch did not reset");
    auto beforeWorld = denoiseStats();
    ++temporal.worldEpoch;
    frame();
    require(denoiseStats().historyResets == beforeWorld.historyResets + 1, "World epoch did not reset");
    ok(api.SetConfigVariable("rtx.native.pathtrace.validation", "1"));
    frame();
    frame();
    require(denoiseStats().validationEnabled, "NRD validation overlay did not activate");
    ok(api.SetConfigVariable("rtx.native.pathtrace.validation", "0"));
    opaque.albedoConstant = {0, 0, 0};
    ok(api.CreateMaterial(&material, &mat));
    auto blackMaterial = capture("black-material.png");
    require(blackMaterial[0] < 5 && blackMaterial[1] < 5 && blackMaterial[2] < 5,
            "Black material retained demodulated light");
    camera.pNext = nullptr;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "raster"));
  frame();
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  require(caps.activeBackend == 0, "Switch to raster");
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  frame();
  int number = 0;
  auto windows = SDL_GetWindows(&number);
  require(number == 1, "Runtime window");
  SDL_SetWindowSize(windows[0], 128, 96);
  SDL_free(windows);
  frame();
  frame();
  ok(api.DestroyLight(sun));
  require(api.DrawLightInstance(sun) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Destroyed distant light accepted");
  ok(api.Shutdown());
  std::puts("Hardware tracing, caching, accumulation, switching and resize passed");
}
