// A Cornell box through the public SDK: white floor, ceiling and back wall, a red
// left and a green right wall, two white blocks and an emissive ceiling panel as
// the only light. Path traced with accumulation by default.
//
// usage: remix-cornell-sample [LIBRARY] [FRAMES] [BACKEND] [SCREENSHOT.png]
//   FRAMES 0 runs until the window closes; BACKEND is pathtrace (default) or raster;
//   SCREENSHOT (an absolute path) saves the last of FRAMES presented frames.
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace {
  remixapi_Interface api = {};
  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_SUCCESS;
  bool check(remixapi_ErrorCode result, const char *what) {
    if (result && !status) {
      std::fprintf(stderr, "%s failed: 0x%x\n", what, unsigned(result));
      status = result;
    }
    return !result;
  }

  struct Vec3 {
    float x, y, z;
  };
  Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
  Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
  Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
  Vec3 normalize(Vec3 v) { return v * (1 / std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z)); }

  // Quads (corners in order around the face) with flat normals.
  struct Geometry {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
      const Vec3 n = normalize(cross(b - a, c - a));
      const auto base = uint32_t(vertices.size());
      const Vec3 corners[4] = {a, b, c, d};
      const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      for (int i = 0; i < 4; ++i) {
        remixapi_HardcodedVertex v = {};
        v.position[0] = corners[i].x;
        v.position[1] = corners[i].y;
        v.position[2] = corners[i].z;
        v.normal[0] = n.x;
        v.normal[1] = n.y;
        v.normal[2] = n.z;
        v.texcoord[0] = uv[i][0];
        v.texcoord[1] = uv[i][1];
        v.color = 0xffffffff;
        vertices.push_back(v);
      }
      for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
        indices.push_back(base + i);
    }
    // An axis-aligned block on the floor, turned about its vertical axis.
    void block(Vec3 center, float width, float depth, float height, float degrees) {
      const float angle = degrees * 3.14159265f / 180, c = std::cos(angle), s = std::sin(angle);
      auto at = [&](float x, float y, float z) {
        return Vec3{center.x + x * c - z * s, center.y + y, center.z + x * s + z * c};
      };
      const float w = width / 2, d = depth / 2;
      const Vec3 p[8] = {at(-w, 0, -d), at(w, 0, -d), at(w, 0, d), at(-w, 0, d),
                         at(-w, height, -d), at(w, height, -d), at(w, height, d), at(-w, height, d)};
      quad(p[4], p[5], p[6], p[7]); // top
      quad(p[0], p[1], p[5], p[4]); // front
      quad(p[1], p[2], p[6], p[5]); // right
      quad(p[2], p[3], p[7], p[6]); // back
      quad(p[3], p[0], p[4], p[7]); // left
    }
  };

  remixapi_MaterialHandle material(uint64_t hash, Vec3 albedo, Vec3 emission = {}, float intensity = 0) {
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {albedo.x, albedo.y, albedo.z};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = 1;
    opaque.alphaTestType = 7; // always
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = hash;
    info.emissiveIntensity = intensity;
    info.emissiveColorConstant = {emission.x, emission.y, emission.z};
    remixapi_MaterialHandle handle = nullptr;
    check(api.CreateMaterial(&info, &handle), "CreateMaterial");
    return handle;
  }

  remixapi_MeshHandle mesh(uint64_t hash, const Geometry &geometry, remixapi_MaterialHandle material) {
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = geometry.vertices.data();
    surface.vertices_count = geometry.vertices.size();
    surface.indices_values = geometry.indices.data();
    surface.indices_count = geometry.indices.size();
    surface.material = material;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_count = 1;
    info.surfaces_values = &surface;
    remixapi_MeshHandle handle = nullptr;
    check(api.CreateMeshBatched(&info, &handle), "CreateMesh");
    return handle;
  }
} // namespace

int main(int argc, char **argv) {
#if defined(_WIN32)
  const std::filesystem::path libraryPathName = argc > 1 ? argv[1] : "remix.dll";
  const wchar_t *libraryPath = libraryPathName.c_str();
#elif defined(__APPLE__)
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.0.dylib";
#else
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.so.0";
#endif
  const unsigned maxFrames = argc > 2 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0;
  const char *backend = argc > 3 ? argv[3] : "pathtrace";
  const char *screenshot = argc > 4 && maxFrames ? argv[4] : nullptr;
  remixapi_HMODULE library = nullptr;
  if (remixapi_lib_loadRemixDllAndInitialize(libraryPath, &api, &library)) {
    std::fprintf(stderr, "Remix load failed: %s\n", argc > 1 ? argv[1] : "default library");
    return 1;
  }
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowTitle = "Remix Cornell Box";
  startup.windowWidth = startup.windowHeight = 900;
#endif
  check(api.Startup(&startup), "Startup");
  if (!status) {
    const auto selected = api.SetConfigVariable("rtx.native.backend", backend);
    if (selected == REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE)
      std::fprintf(stderr, "Path tracing unavailable; rasterizing\n");
    else
      check(selected, "Select backend");
    // A converging reference image: accumulation and a few bounces; the
    // emissive panel is the only light.
    // ACCUMULATE=0 shows the real-time denoised image (NRD, TAA, SHARC) instead.
    const char *accumulate = std::getenv("ACCUMULATE");
    api.SetConfigVariable("rtx.native.pathtrace.accumulate", accumulate ? accumulate : "1");
    api.SetConfigVariable("rtx.native.pathtrace.bounces", "4");
    api.SetConfigVariable("rtx.native.pathtrace.emissiveLights", "1");
    api.SetConfigVariable("rtx.native.pathtrace.overrideLight", "0");
  }

  // The box spans x -1..1, y -1..1, z 0..2, open towards the camera at z 0.
  std::vector<std::pair<remixapi_MeshHandle, bool>> meshes;
  if (!status) {
    const auto white = material(0xc0, {.73f, .73f, .73f});
    const auto red = material(0xc1, {.63f, .065f, .05f});
    const auto green = material(0xc2, {.14f, .45f, .091f});
    const auto light = material(0xc3, {.78f, .78f, .78f}, {1, .85f, .63f}, 15);
    Geometry walls, left, right, blocks, panel;
    walls.quad({-1, -1, 0}, {1, -1, 0}, {1, -1, 2}, {-1, -1, 2}); // floor
    walls.quad({-1, 1, 2}, {1, 1, 2}, {1, 1, 0}, {-1, 1, 0});     // ceiling
    walls.quad({-1, -1, 2}, {1, -1, 2}, {1, 1, 2}, {-1, 1, 2});   // back
    left.quad({-1, -1, 0}, {-1, -1, 2}, {-1, 1, 2}, {-1, 1, 0});
    right.quad({1, -1, 2}, {1, -1, 0}, {1, 1, 0}, {1, 1, 2});
    blocks.block({-.33f, -1, 1.3f}, .6f, .6f, 1.2f, 17);  // tall
    blocks.block({.35f, -1, .65f}, .6f, .6f, .6f, -18);   // short
    // Just below the ceiling, so it never coincides with it.
    panel.quad({-.25f, .995f, .75f}, {.25f, .995f, .75f}, {.25f, .995f, 1.25f}, {-.25f, .995f, 1.25f});
    meshes = {{mesh(0xd0, walls, white), true},
              {mesh(0xd1, left, red), true},
              {mesh(0xd2, right, green), true},
              {mesh(0xd3, blocks, white), true},
              {mesh(0xd4, panel, light), true}};
  }
  if (!status)
    std::fprintf(stderr, "Alt+X: settings panel (the runtime's shortcut; set it in the panel's header)\n");

  // One frame: camera, instances and presentation (with the capture request).
  auto drawFrame = [&](const remixapi_WindowState &state, bool capture = false) {
    // Left-handed camera (right +X, up +Y, forward +Z) looking into the box.
    remixapi_CameraInfoParameterizedEXT parameters = {};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.position = {0, 0, -2.75f};
    parameters.forward = {0, 0, 1};
    parameters.up = {0, 1, 0};
    parameters.right = {1, 0, 0};
    parameters.aspect = state.drawableHeight ? float(state.drawableWidth) / state.drawableHeight : 1;
    parameters.fovYInDegrees = 40;
    parameters.nearPlane = .05f;
    parameters.farPlane = 100;
    remixapi_CameraInfo camera = {};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    check(api.SetupCamera(&camera), "SetupCamera");
    for (size_t i = 0; i < meshes.size(); ++i) {
      const auto &[handle, doubleSided] = meshes[i];
      // Static instances: the radiance cache (SHARC) caches static receivers.
      remixapi_InstanceIdentityEXT identity = {};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = i + 1;
      identity.classification = 0;
      identity.rasterVisible = true;
      remixapi_InstanceInfo instance = {};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &identity;
      instance.mesh = handle;
      // Walls are seen from inside: double-sided, as in the classic scene.
      instance.doubleSided = doubleSided;
      instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
      check(api.DrawInstance(&instance), "DrawInstance");
    }
    if (capture)
      check(api.RequestPresentedScreenshot(screenshot), "RequestPresentedScreenshot");
    if (!status)
      check(api.Present(nullptr), "Present");
  };
  for (unsigned frame = 0; !status && (!maxFrames || frame < maxFrames); ++frame) {
    if (!check(api.PumpEvents(), "PumpEvents"))
      break;
    remixapi_WindowState state = {};
    if (!check(api.GetWindowState(&state), "GetWindowState") || state.closeRequested)
      break;
    drawFrame(state, screenshot && frame + 1 == maxFrames);
    std::this_thread::sleep_for(std::chrono::milliseconds(state.drawableWidth ? 1 : 50));
  }
  // The capture is written once a presented frame completes; frames the
  // window system did not present (an occluded window) leave it pending.
  for (unsigned extra = 0; screenshot && !status && extra < 600 && !std::filesystem::exists(screenshot); ++extra) {
    check(api.PumpEvents(), "PumpEvents");
    remixapi_WindowState state = {};
    check(api.GetWindowState(&state), "GetWindowState");
    drawFrame(state);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto shutdown = remixapi_lib_shutdownAndUnloadRemixDll(&api, library);
  if (status || shutdown) {
    std::fprintf(stderr, "Remix runtime failed: 0x%x, shutdown: 0x%x\n", unsigned(status), unsigned(shutdown));
    return 1;
  }
  return 0;
}
