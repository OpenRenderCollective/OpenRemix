// Crytek Sponza through the public SDK: the OBJ/MTL (Morgan McGuire's edition,
// fetched by scripts/common/fetch-sponza.py) as one static mesh with a surface
// per material, alpha-tested foliage and chains, and a sun through the atrium.
// Real-time path tracing (denoising, SHARC) by default, with frame timings
// printed every two seconds.
//
// usage: remix-sponza-sample [LIBRARY] [FRAMES] [BACKEND] [SPONZA.obj] [SCREENSHOT.png]
//   FRAMES 0 runs until the window closes; BACKEND is pathtrace (default) or raster;
//   SPONZA.obj defaults to _work/assets/sponza/sponza.obj; SCREENSHOT (an absolute
//   path) saves the last of FRAMES presented frames.
//   ACCUMULATE=1 converges a reference image; MOVE=1 moves the camera along the
//   nave (temporal reprojection under motion).
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {
  using Clock = std::chrono::steady_clock;
  remixapi_Interface api = {};
  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_SUCCESS;
  bool check(remixapi_ErrorCode result, const char *what) {
    if (result && !status) {
      std::fprintf(stderr, "%s failed: 0x%x\n", what, unsigned(result));
      status = result;
    }
    return !result;
  }

  struct Material {
    std::string name, albedo; // albedo: an absolute texture path or empty
    float color[3] = {1, 1, 1};
    bool masked = false;      // map_d: the mask is in the albedo's alpha
  };
  struct Surface {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    std::map<std::tuple<int, int, int>, uint32_t> lookup;
  };

  // MTL texture paths are relative to the MTL with Windows separators.
  std::string texturePath(const std::filesystem::path &directory, std::string relative) {
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return (directory / relative).lexically_normal().string();
  }

  std::vector<Material> loadMaterials(const std::filesystem::path &path) {
    std::vector<Material> materials;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
      std::istringstream in(line);
      std::string key;
      in >> key;
      if (key == "newmtl") {
        materials.emplace_back();
        in >> materials.back().name;
      } else if (materials.empty())
        continue;
      else if (key == "Kd")
        in >> materials.back().color[0] >> materials.back().color[1] >> materials.back().color[2];
      else if (key == "map_Kd") {
        std::string relative;
        in >> relative;
        materials.back().albedo = texturePath(path.parent_path(), relative);
      } else if (key == "map_d")
        materials.back().masked = true;
    }
    return materials;
  }

  // OBJ (right-handed, Y up, centimeters) into the API's left-handed meters:
  // Z mirrored, so triangle winding reverses; V flipped for top-down images.
  bool loadObj(const std::filesystem::path &path, std::vector<Material> &materials, std::vector<Surface> &surfaces) {
    std::ifstream file(path);
    if (!file)
      return false;
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<float, 2>> texcoords;
    std::unordered_map<std::string, size_t> materialIndex;
    size_t current = 0;
    std::string line;
    std::vector<std::tuple<int, int, int>> corners;
    while (std::getline(file, line)) {
      if (line.size() < 2)
        continue;
      const char *text = line.c_str();
      if (line[0] == 'v' && line[1] == ' ') {
        std::array<float, 3> p;
        std::sscanf(text + 2, "%f %f %f", &p[0], &p[1], &p[2]);
        positions.push_back({p[0] * .01f, p[1] * .01f, -p[2] * .01f});
      } else if (line[0] == 'v' && line[1] == 't') {
        std::array<float, 2> t = {};
        std::sscanf(text + 3, "%f %f", &t[0], &t[1]);
        texcoords.push_back({t[0], 1 - t[1]});
      } else if (line[0] == 'v' && line[1] == 'n') {
        std::array<float, 3> n;
        std::sscanf(text + 3, "%f %f %f", &n[0], &n[1], &n[2]);
        normals.push_back({n[0], n[1], -n[2]});
      } else if (line.compare(0, 7, "mtllib ") == 0) {
        materials = loadMaterials(path.parent_path() / line.substr(7, line.find_last_not_of(" \r") - 6));
        surfaces.assign(materials.size(), {});
        for (size_t i = 0; i < materials.size(); ++i)
          materialIndex[materials[i].name] = i;
      } else if (line.compare(0, 7, "usemtl ") == 0) {
        const auto found = materialIndex.find(line.substr(7, line.find_last_not_of(" \r") - 6));
        current = found == materialIndex.end() ? 0 : found->second;
      } else if (line[0] == 'f' && line[1] == ' ' && !surfaces.empty()) {
        corners.clear();
        std::istringstream in(line.substr(2));
        std::string corner;
        while (in >> corner) {
          int v = 0, t = 0, n = 0;
          if (std::sscanf(corner.c_str(), "%d/%d/%d", &v, &t, &n) < 3 &&
              std::sscanf(corner.c_str(), "%d//%d", &v, &n) < 2)
            std::sscanf(corner.c_str(), "%d/%d", &v, &t);
          // Negative indices count back from the latest element.
          auto resolve = [](int index, size_t count) { return index < 0 ? int(count) + index : index - 1; };
          corners.emplace_back(resolve(v, positions.size()), t ? resolve(t, texcoords.size()) : -1,
                               n ? resolve(n, normals.size()) : -1);
        }
        auto &surface = surfaces[current];
        auto vertex = [&](const std::tuple<int, int, int> &key) {
          const auto [found, inserted] = surface.lookup.try_emplace(key, uint32_t(surface.vertices.size()));
          if (inserted) {
            remixapi_HardcodedVertex v = {};
            const auto &[p, t, n] = key;
            std::memcpy(v.position, positions[p].data(), sizeof(v.position));
            if (t >= 0)
              std::memcpy(v.texcoord, texcoords[t].data(), sizeof(v.texcoord));
            if (n >= 0)
              std::memcpy(v.normal, normals[n].data(), sizeof(v.normal));
            v.color = 0xffffffff;
            surface.vertices.push_back(v);
          }
          return found->second;
        };
        // A fan with reversed winding (the mirror).
        for (size_t i = 2; i < corners.size(); ++i)
          for (const auto &corner : {corners[0], corners[i], corners[i - 1]})
            surface.indices.push_back(vertex(corner));
      }
    }
    return !surfaces.empty();
  }
} // namespace

int main(int argc, char **argv) {
#if defined(_WIN32)
  std::fprintf(stderr, "remix-sponza-sample uses UTF-8 texture paths (not Windows)\n");
  return 1;
#else
#if defined(__APPLE__)
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.0.dylib";
#else
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.so.0";
#endif
  const unsigned maxFrames = argc > 2 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0;
  const char *backend = argc > 3 ? argv[3] : "pathtrace";
  const std::filesystem::path objPath =
      std::filesystem::absolute(argc > 4 && *argv[4] ? argv[4] : "_work/assets/sponza/sponza.obj");
  const char *screenshot = argc > 5 && maxFrames ? argv[5] : nullptr;

  std::vector<Material> materials;
  std::vector<Surface> surfaces;
  const auto loadStart = Clock::now();
  if (!loadObj(objPath, materials, surfaces)) {
    std::fprintf(stderr, "Cannot read %s: run scripts/common/fetch-sponza.py\n", objPath.string().c_str());
    return 1;
  }
  size_t triangles = 0;
  for (const auto &surface : surfaces)
    triangles += surface.indices.size() / 3;
  std::fprintf(stderr, "Sponza: %zu materials, %zu triangles, parsed in %.0f ms\n", materials.size(), triangles,
               std::chrono::duration<double, std::milli>(Clock::now() - loadStart).count());

  remixapi_HMODULE library = nullptr;
  if (remixapi_lib_loadRemixDllAndInitialize(libraryPath, &api, &library)) {
    std::fprintf(stderr, "Remix load failed: %s\n", libraryPath);
    return 1;
  }
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.windowTitle = "Remix Sponza";
  // WINDOW=WxH: the window size in points (a Retina drawable is twice that).
  if (const char *window = std::getenv("WINDOW"))
    std::sscanf(window, "%ux%u", &startup.windowWidth, &startup.windowHeight);
  check(api.Startup(&startup), "Startup");
  if (!status) {
    const auto selected = api.SetConfigVariable("rtx.native.backend", backend);
    if (selected == REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE)
      std::fprintf(stderr, "Path tracing unavailable; rasterizing\n");
    else
      check(selected, "Select backend");
    // Timings measure the renderer, not the display.
    api.SetConfigVariable("rtx.native.vsync", "0");
    const char *accumulate = std::getenv("ACCUMULATE");
    api.SetConfigVariable("rtx.native.pathtrace.accumulate", accumulate ? accumulate : "0");
    // SETTINGS=key=value,...: settings for a benchmark run, over the profile's.
    if (const char *settings = std::getenv("SETTINGS")) {
      std::string list = settings;
      for (size_t begin = 0; begin < list.size();) {
        const size_t end = std::min(list.find(',', begin), list.size());
        const std::string pair = list.substr(begin, end - begin);
        if (const size_t equals = pair.find('='); equals != std::string::npos &&
            api.SetConfigVariable(pair.substr(0, equals).c_str(), pair.substr(equals + 1).c_str()))
          std::fprintf(stderr, "Setting rejected: %s\n", pair.c_str());
        begin = end + 1;
      }
    }
  }

  // The sun, high and to the side: it lights one gallery and the floor
  // through the open roof; the rest is indirect.
  remixapi_LightHandle sun = nullptr;
  if (!status) {
    remixapi_LightInfoDistantEXT distant = {};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {.2297f, -.9186f, .3215f}; // normalized
    distant.angularDiameterDegrees = .5f;
    remixapi_LightInfo light = {};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &distant;
    light.hash = 0x5100;
    light.radiance = {9, 8.5f, 7.5f};
    check(api.CreateLight(&light, &sun), "CreateLight");
  }

  remixapi_MeshHandle mesh = nullptr;
  if (!status) {
    std::vector<remixapi_MeshInfoSurfaceTriangles> parts;
    for (size_t i = 0; i < materials.size(); ++i) {
      if (surfaces[i].indices.empty())
        continue;
      const auto &m = materials[i];
      remixapi_MaterialInfoOpaqueEXT opaque = {};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.albedoConstant = {m.color[0], m.color[1], m.color[2]};
      opaque.opacityConstant = 1;
      opaque.roughnessConstant = .7f;
      // Foliage and chains: alpha greater than one half.
      opaque.alphaTestType = m.masked ? 4 : 7;
      opaque.alphaReferenceValue = 127;
      remixapi_MaterialInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &opaque;
      info.hash = 0x5200 + i;
      info.albedoTexture = m.albedo.empty() ? nullptr : m.albedo.c_str();
      info.wrapModeU = info.wrapModeV = 1; // repeat
      remixapi_MaterialHandle handle = nullptr;
      if (!check(api.CreateMaterial(&info, &handle), "CreateMaterial"))
        break;
      remixapi_MeshInfoSurfaceTriangles surface = {};
      surface.vertices_values = surfaces[i].vertices.data();
      surface.vertices_count = surfaces[i].vertices.size();
      surface.indices_values = surfaces[i].indices.data();
      surface.indices_count = surfaces[i].indices.size();
      surface.material = handle;
      parts.push_back(surface);
    }
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = 0x5300;
    info.surfaces_count = uint32_t(parts.size());
    info.surfaces_values = parts.data();
    if (!status)
      check(api.CreateMeshBatched(&info, &mesh), "CreateMesh");
  }
  if (!status)
    std::fprintf(stderr, "Alt+X: settings panel (free camera: Rendering tab; the shortcut is set in the panel's header)\n");

  using Tracing = remixapi_ErrorCode(REMIXAPI_CALL *)(remixapi_TracingStatisticsEXT *);
  const auto tracingOf = reinterpret_cast<Tracing>(dlsym(library, "remixapi_GetTracingStatisticsEXT"));
  const bool move = std::getenv("MOVE") && std::strcmp(std::getenv("MOVE"), "0") != 0;
  const auto start = Clock::now();
  auto windowStart = Clock::now();
  unsigned windowFrames = 0;
  double gpuSum = 0;

  auto drawFrame = [&](const remixapi_WindowState &state, bool capture = false) {
    // Down the nave from its east end, at eye height, looking west.
    const float t = std::chrono::duration<float>(Clock::now() - start).count();
    const float x = move ? 3 + 6 * std::cos(t * .4f) : 9.5f;
    remixapi_CameraInfoParameterizedEXT parameters = {};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.position = {x, 1.7f, -.4f};
    parameters.forward = {-1, .06f, 0};
    parameters.up = {.06f, 1, 0};
    parameters.right = {0, 0, 1};
    parameters.aspect = state.drawableHeight ? float(state.drawableWidth) / state.drawableHeight : 1;
    parameters.fovYInDegrees = 65;
    parameters.nearPlane = .05f;
    parameters.farPlane = 200;
    remixapi_CameraInfo camera = {};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    check(api.SetupCamera(&camera), "SetupCamera");
    check(api.DrawLightInstance(sun), "DrawLightInstance");
    // Static: the radiance cache (SHARC) caches static receivers. Double-sided
    // for the curtains and foliage cards.
    remixapi_InstanceIdentityEXT identity = {};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 1;
    identity.classification = 0;
    identity.rasterVisible = true;
    remixapi_InstanceInfo instance = {};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &identity;
    instance.mesh = mesh;
    instance.doubleSided = 1;
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
    check(api.DrawInstance(&instance), "DrawInstance");
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
    if (!state.drawableWidth) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    // Every two seconds: presented frame rate and the GPU's tracing time.
    remixapi_TracingStatisticsEXT tracing = {.structSize = sizeof(tracing)};
    if (tracingOf && !tracingOf(&tracing))
      gpuSum += tracing.gpuTraceMilliseconds;
    ++windowFrames;
    const double elapsed = std::chrono::duration<double>(Clock::now() - windowStart).count();
    if (elapsed >= 2) {
      std::fprintf(stderr, "%ux%u: %.1f fps, frame %.2f ms, GPU trace %.2f ms\n", state.drawableWidth,
                   state.drawableHeight, windowFrames / elapsed, elapsed * 1000 / windowFrames,
                   gpuSum / windowFrames);
      windowStart = Clock::now();
      windowFrames = 0;
      gpuSum = 0;
    }
  }
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
#endif
}
