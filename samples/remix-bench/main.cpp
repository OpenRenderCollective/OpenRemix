// Repeatable renderer benchmark built only on the public SDK.
//   remix-bench LIBRARY FRAMES BACKEND [grid=N] [foliage=N] [textures=N] [size=N] [format=bc|rgba] [stats=0|1] [work=MS] [async=0|1] [churn=N] [churnTris=N]
// Draws a grid of one-sided textured cubes and double-sided alpha-tested
// foliage cards, then reports texture creation, first frame and steady Present
// timings. Measurement starts once the requested backend reports GPU timings
// (background pipeline preparation presents raster until then). Combine with
// REMIX_STACK_PROFILE / REMIX_RAY_PROFILE for per-pass GPU detail.
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace {
  using Clock = std::chrono::steady_clock;
  double milliseconds(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

  struct Options {
    unsigned grid = 48, foliage = 4000, textures = 64, size = 512;
    bool compressed = true;
    // Statistics getters wait for asynchronous rendering; stats=0 stops polling
    // them once the backend is ready so presentation can overlap.
    bool stats = true;
    // Simulated game-thread CPU work per frame (milliseconds), outside the API.
    unsigned work = 0;
    // rtx.native.asyncPresent: render on a separate thread while the next frame is built.
    bool async = false;
    // Meshes re-created every frame under their hash with new vertices (MESH_UPDATE_EXT
    // with motion, as a capture host updates animated geometry), churnTris triangles each.
    unsigned churn = 0, churnTris = 5000;
  };

  uint16_t rgb565(unsigned r, unsigned g, unsigned b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); }

  // Mip chain of BC1 (opaque) or BC3 (alpha disc per 64-texel cell) blocks.
  std::vector<uint8_t> blockTexture(unsigned size, unsigned mips, unsigned seed, bool alpha) {
    std::vector<uint8_t> data;
    for (unsigned mip = 0; mip < mips; ++mip) {
      const unsigned extent = std::max(1u, size >> mip), blocks = std::max(1u, extent / 4);
      const unsigned cell = std::max(4u, 64u >> mip);
      for (unsigned by = 0; by < blocks; ++by)
        for (unsigned bx = 0; bx < blocks; ++bx) {
          if (alpha) {
            uint8_t block[8] = {255, 0};
            uint64_t bits = 0;
            for (unsigned t = 0; t < 16; ++t) {
              const float x = float((bx * 4 + t % 4) % cell) / cell - .5f, y = float((by * 4 + t / 4) % cell) / cell - .5f;
              bits |= uint64_t(x * x + y * y < .2f ? 0 : 1) << (t * 3);
            }
            std::memcpy(block + 2, &bits, 6);
            data.insert(data.end(), block, block + 8);
          }
          const uint16_t c0 = rgb565(60 + seed * 37 % 160, 90 + seed * 53 % 140, 40 + (bx ^ by) % 2 * 60), c1 = rgb565(20, 30, 20);
          const uint32_t indices = (bx + by + seed) % 3 ? 0x00000000u : 0x55555555u;
          uint8_t color[8];
          std::memcpy(color, &c0, 2);
          std::memcpy(color + 2, &c1, 2);
          std::memcpy(color + 4, &indices, 4);
          data.insert(data.end(), color, color + 8);
        }
    }
    return data;
  }

  std::vector<uint8_t> rgbaTexture(unsigned size, unsigned mips, unsigned seed, bool alpha) {
    std::vector<uint8_t> data;
    for (unsigned mip = 0; mip < mips; ++mip) {
      const unsigned extent = std::max(1u, size >> mip), cell = std::max(1u, 64u >> mip);
      for (unsigned y = 0; y < extent; ++y)
        for (unsigned x = 0; x < extent; ++x) {
          const float u = float(x % cell) / cell - .5f, v = float(y % cell) / cell - .5f;
          const uint8_t texel[4] = {uint8_t(60 + seed * 37 % 160), uint8_t(90 + seed * 53 % 140),
                                    uint8_t(40 + ((x / 4) ^ (y / 4)) % 2 * 60),
                                    uint8_t(!alpha || u * u + v * v < .2f ? 255 : 0)};
          data.insert(data.end(), texel, texel + 4);
        }
    }
    return data;
  }

  struct Mesh {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    void quad(const float (&p)[4][3], float u0 = 0, float u1 = 1) {
      const uint32_t base = uint32_t(vertices.size());
      for (unsigned v = 0; v < 4; ++v) {
        remixapi_HardcodedVertex vertex = {};
        std::memcpy(vertex.position, p[v], sizeof(p[v]));
        vertex.texcoord[0] = v == 1 || v == 2 ? u1 : u0;
        vertex.texcoord[1] = v >= 2 ? 1.f : 0.f;
        vertex.color = 0xffffffff;
        vertices.push_back(vertex);
      }
      for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
        indices.push_back(base + i);
    }
  };

  Mesh cube() {
    constexpr float c[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                               {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    constexpr unsigned faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3},
                                      {1, 2, 6, 5}, {3, 7, 6, 2}, {0, 1, 5, 4}};
    Mesh mesh;
    for (const auto &face : faces) {
      float p[4][3];
      for (unsigned v = 0; v < 4; ++v)
        std::memcpy(p[v], c[face[v]], sizeof(p[v]));
      mesh.quad(p);
    }
    return mesh;
  }

  // Two crossed cards, each subdivided so alpha tests dominate traversal.
  Mesh cards() {
    Mesh mesh;
    for (unsigned card = 0; card < 2; ++card)
      for (unsigned s = 0; s < 4; ++s) {
        const float a = -1 + s * .5f, b = a + .5f;
        float p[4][3] = {{a, 0, 0}, {b, 0, 0}, {b, 2, 0}, {a, 2, 0}};
        if (card)
          for (auto &v : p)
            std::swap(v[0], v[2]);
        mesh.quad(p, (a + 1) * .5f, (b + 1) * .5f);
      }
    return mesh;
  }

  float random(uint32_t &state) {
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / 16777216.f;
  }

  struct Stats {
    double mean = 0, p50 = 0, p95 = 0;
  };
  Stats summarize(std::vector<double> values) {
    Stats s;
    if (values.empty())
      return s;
    std::sort(values.begin(), values.end());
    for (double v : values)
      s.mean += v / values.size();
    s.p50 = values[values.size() / 2];
    s.p95 = values[std::min(values.size() - 1, values.size() * 95 / 100)];
    return s;
  }
} // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s LIBRARY FRAMES BACKEND [grid=N] [foliage=N] [textures=N] [size=N] [format=bc|rgba] [stats=0|1] [work=MS] [async=0|1] [churn=N] [churnTris=N]\n", argv[0]);
    return 2;
  }
  Options options;
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto eq = arg.find('=');
    const std::string key = arg.substr(0, eq), value = eq == std::string::npos ? "" : arg.substr(eq + 1);
    const unsigned number = unsigned(std::strtoul(value.c_str(), nullptr, 10));
    if (key == "grid") options.grid = number;
    else if (key == "foliage") options.foliage = number;
    else if (key == "textures") options.textures = std::max(1u, number);
    else if (key == "size") options.size = std::max(4u, number);
    else if (key == "format") options.compressed = value != "rgba";
    else if (key == "stats") options.stats = value != "0";
    else if (key == "work") options.work = number;
    else if (key == "async") options.async = value != "0";
    else if (key == "churn") options.churn = number;
    else if (key == "churnTris") options.churnTris = std::max(2u, number);
    else {
      std::fprintf(stderr, "unknown option %s\n", arg.c_str());
      return 2;
    }
  }
  const unsigned frames = unsigned(std::strtoul(argv[2], nullptr, 10)), warmup = 30;

  remixapi_Interface api = {};
  remixapi_HMODULE library = nullptr;
  // The SDK takes the library path in the platform's encoding (wide on Windows).
  const std::filesystem::path libraryPath = argv[1];
  auto status = remixapi_lib_loadRemixDllAndInitialize(libraryPath.c_str(), &api, &library);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "Remix load failed: 0x%x\n", unsigned(status));
    return 1;
  }
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32) // On Windows the runtime creates its own window (hwnd stays null).
  startup.windowTitle = "Remix Bench";
#endif
  status = api.Startup(&startup);
  auto check = [&](remixapi_ErrorCode result, const char *what) {
    if (result != REMIXAPI_ERROR_CODE_SUCCESS && status == REMIXAPI_ERROR_CODE_SUCCESS) {
      std::fprintf(stderr, "%s failed: 0x%x\n", what, unsigned(result));
      status = result;
    }
    return status == REMIXAPI_ERROR_CODE_SUCCESS;
  };
  check(api.SetConfigVariable("rtx.native.vsync", "0"), "vsync");
  check(api.SetConfigVariable("rtx.native.backend", argv[3]), "backend");
  check(api.SetConfigVariable("rtx.native.asyncPresent", options.async ? "1" : "0"), "async");
  const bool tracing = std::strcmp(argv[3], "pathtrace") == 0;
#if defined(_WIN32)
  auto symbol = [&](const char *name) { return GetProcAddress(library, name); };
#else
  auto symbol = [&](const char *name) { return dlsym(library, name); };
#endif
  using Tracing = remixapi_ErrorCode(REMIXAPI_CALL *)(remixapi_TracingStatisticsEXT *);
  using Performance = remixapi_ErrorCode(REMIXAPI_CALL *)(remixapi_FramePerformanceEXT *);
  const auto tracingStatistics = reinterpret_cast<Tracing>(symbol("remixapi_GetTracingStatisticsEXT"));
  const auto framePerformance = reinterpret_cast<Performance>(symbol("remixapi_GetFramePerformanceEXT"));
  using PassTimings = remixapi_ErrorCode(REMIXAPI_CALL *)(remixapi_GpuPassTimingsEXT *);
  const auto passTimings = reinterpret_cast<PassTimings>(symbol("remixapi_GetGpuPassTimingsEXT"));
  if (passTimings)
    check(api.SetConfigVariable("rtx.native.gpuPassTimers", "1"), "gpuPassTimers");
  if (!tracingStatistics || !framePerformance)
    check(REMIXAPI_ERROR_CODE_GENERAL_FAILURE, "statistics exports");

  // Textures: one opaque surface texture per slot, plus one foliage cutout.
  unsigned mips = 1;
  while ((options.size >> mips) >= 4)
    ++mips;
  std::vector<remixapi_TextureHandle> textures;
  std::vector<remixapi_MaterialHandle> materials;
  std::vector<remixapi_MeshHandle> meshes;
  uint64_t textureBytes = 0;
  const auto textureStart = Clock::now();
  for (unsigned t = 0; status == REMIXAPI_ERROR_CODE_SUCCESS && t <= options.textures; ++t) {
    const bool alpha = t == options.textures;
    const auto data = options.compressed ? blockTexture(options.size, mips, t, alpha) : rgbaTexture(options.size, mips, t, alpha);
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = 0x1000 + t;
    info.width = info.height = options.size;
    info.depth = 1;
    info.mipLevels = mips;
    info.format = options.compressed ? (alpha ? REMIXAPI_FORMAT_BC3_SRGB : REMIXAPI_FORMAT_BC1_RGB_SRGB)
                                     : REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    info.data = data.data();
    info.dataSize = data.size();
    textureBytes += data.size();
    textures.emplace_back();
    check(api.CreateTexture(&info, &textures.back()), "CreateTexture");
  }
  const double textureMs = milliseconds(Clock::now() - textureStart);

  const Mesh shapes[2] = {cube(), cards()};
  for (unsigned t = 0; status == REMIXAPI_ERROR_CODE_SUCCESS && t <= options.textures; ++t) {
    const bool alpha = t == options.textures;
    char hash[32];
    std::snprintf(hash, sizeof(hash), "0x%llx", static_cast<unsigned long long>(0x1000 + t));
    // A texture's hash as its path, in the platform's encoding (wide on Windows).
    const std::filesystem::path path = hash;
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = .6f;
    opaque.alphaTestType = alpha ? 4 : 7; // greater
    opaque.alphaReferenceValue = 127;
    remixapi_MaterialInfo material = {};
    material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    material.pNext = &opaque;
    material.hash = 0x2000 + t;
    material.albedoTexture = path.c_str();
    material.wrapModeU = material.wrapModeV = 1;
    materials.emplace_back();
    if (!check(api.CreateMaterial(&material, &materials.back()), "CreateMaterial"))
      break;
    const auto &shape = shapes[alpha];
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = shape.vertices.data();
    surface.vertices_count = shape.vertices.size();
    surface.indices_values = shape.indices.data();
    surface.indices_count = shape.indices.size();
    surface.material = materials.back();
    remixapi_MeshInfo mesh = {};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = 0x3000 + t;
    mesh.surfaces_count = 1;
    mesh.surfaces_values = &surface;
    meshes.emplace_back();
    check(api.CreateMeshBatched(&mesh, &meshes.back()), "CreateMesh");
  }

  remixapi_LightHandle sun = nullptr;
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    remixapi_LightInfoDistantEXT distant = {};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {-.4f, -.8f, -.3f};
    distant.angularDiameterDegrees = .5f;
    remixapi_LightInfo light = {};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &distant;
    light.hash = 0x4000;
    light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
    check(api.CreateLight(&light, &sun), "CreateLight");
  }

  // Churn: a grid sheet per mesh, re-created each frame with displaced vertices.
  const unsigned churnSide = unsigned(std::ceil(std::sqrt(options.churnTris / 2.0)));
  std::vector<uint32_t> churnIndices;
  for (unsigned y = 0; y < churnSide; ++y)
    for (unsigned x = 0; x < churnSide; ++x) {
      const uint32_t a = y * (churnSide + 1) + x, b = a + 1, c = a + churnSide + 1, d = c + 1;
      churnIndices.insert(churnIndices.end(), {a, c, b, b, c, d});
    }
  std::vector<remixapi_HardcodedVertex> churnVertices((churnSide + 1) * (churnSide + 1));
  std::vector<remixapi_MeshHandle> churnMeshes(options.churn);
  auto churnMesh = [&](unsigned index, unsigned frame) {
    for (unsigned y = 0; y <= churnSide; ++y)
      for (unsigned x = 0; x <= churnSide; ++x) {
        auto &v = churnVertices[y * (churnSide + 1) + x];
        v = {};
        v.position[0] = float(x) / churnSide * 2 - 1;
        v.position[2] = float(y) / churnSide * 2 - 1;
        v.position[1] = .1f * std::sin(frame * .1f + x * .3f + y * .2f + index);
        v.normal[1] = 1;
        v.texcoord[0] = float(x) / churnSide;
        v.texcoord[1] = float(y) / churnSide;
        v.color = 0xffffffffu;
      }
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = churnVertices.data();
    surface.vertices_count = churnVertices.size();
    surface.indices_values = churnIndices.data();
    surface.indices_count = churnIndices.size();
    surface.material = materials.front();
    remixapi_MeshInfoMotionEXT motion = {};
    motion.sType = REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT;
    motion.enabled = 1;
    remixapi_MeshInfoUpdateEXT update = {};
    update.sType = REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT;
    update.enabled = 1;
    update.pNext = &motion;
    remixapi_MeshInfo mesh = {};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.pNext = &update;
    mesh.hash = 0x5000 + index;
    mesh.surfaces_count = 1;
    mesh.surfaces_values = &surface;
    return check(api.CreateMesh(&mesh, &churnMeshes[index]), "CreateMesh churn");
  };
  for (unsigned i = 0; i < options.churn && status == REMIXAPI_ERROR_CODE_SUCCESS && !materials.empty(); ++i)
    churnMesh(i, 0);
  std::vector<double> uploadPass, assemblyPass, gpuFrame;
  uint64_t lastPassFrame = 0;
  // Fixed layout: static cubes on a grid with foliage clumps between them.
  struct Placement {
    remixapi_MeshHandle mesh;
    remixapi_Transform transform;
    bool doubleSided;
  };
  std::vector<Placement> placements;
  uint32_t seed = 7;
  const float spacing = 4, half = options.grid * spacing * .5f;
  for (unsigned z = 0; z < options.grid && !meshes.empty(); ++z)
    for (unsigned x = 0; x < options.grid; ++x) {
      Placement p{meshes[(x * 7 + z * 13) % options.textures], {}, false};
      const float scale = .6f + random(seed);
      p.transform.matrix[0][0] = p.transform.matrix[2][2] = scale;
      p.transform.matrix[1][1] = scale * (1 + 2 * random(seed));
      p.transform.matrix[0][3] = x * spacing - half;
      p.transform.matrix[1][3] = p.transform.matrix[1][1] - 1;
      p.transform.matrix[2][3] = z * spacing + 4;
      placements.push_back(p);
    }
  for (unsigned i = 0; i < options.foliage && !meshes.empty(); ++i) {
    Placement p{meshes.back(), {}, true};
    const float angle = random(seed) * 6.2831853f, scale = .5f + random(seed);
    p.transform.matrix[0][0] = std::cos(angle) * scale;
    p.transform.matrix[0][2] = std::sin(angle) * scale;
    p.transform.matrix[1][1] = scale;
    p.transform.matrix[2][0] = -std::sin(angle) * scale;
    p.transform.matrix[2][2] = std::cos(angle) * scale;
    p.transform.matrix[0][3] = (random(seed) * 2 - 1) * half;
    p.transform.matrix[1][3] = -1;
    p.transform.matrix[2][3] = 4 + random(seed) * options.grid * spacing;
    placements.push_back(p);
  }

  std::vector<double> presents, framesMs, gpuTrace, gpuBuild;
  // CPU phases of Present, as reported by the runtime.
  const char *phaseNames[] = {"scene", "record", "geometry", "upload", "accel", "fence", "acquire", "submit", "queue_present"};
  std::vector<double> phases[9];
  double firstFrameMs = 0, readyMs = 0, longestMs = 0; // longest: any frame, including upload hitches
  auto previous = Clock::now();
  const auto begin = previous;
  unsigned measured = 0, ready = 0;
  for (unsigned frame = 0; status == REMIXAPI_ERROR_CODE_SUCCESS && measured < frames; ++frame) {
    if (!check(api.PumpEvents(), "PumpEvents"))
      break;
    for (const auto until = Clock::now() + std::chrono::milliseconds(options.work); Clock::now() < until;) {
    }
    remixapi_WindowState window = {};
    if (!check(api.GetWindowState(&window), "GetWindowState") || window.closeRequested)
      break;
    check(api.DrawLightInstance(sun), "DrawLightInstance");
    remixapi_CameraInfoParameterizedEXT params = {};
    params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    const float sway = std::sin(frame * .01f) * .15f;
    params.position = {0, 3, -2};
    params.forward = {std::sin(sway), -.25f, std::cos(sway)};
    const float length = std::hypot(params.forward.x, params.forward.y, params.forward.z);
    params.forward = {params.forward.x / length, params.forward.y / length, params.forward.z / length};
    params.right = {std::cos(sway), 0, -std::sin(sway)};
    params.up = {params.forward.y * params.right.z - params.forward.z * params.right.y,
                 params.forward.z * params.right.x - params.forward.x * params.right.z,
                 params.forward.x * params.right.y - params.forward.y * params.right.x};
    if (params.up.y < 0)
      params.up = {-params.up.x, -params.up.y, -params.up.z};
    params.aspect = window.drawableHeight ? float(window.drawableWidth) / window.drawableHeight : 1;
    params.fovYInDegrees = 70;
    params.nearPlane = .05f;
    params.farPlane = 500;
    remixapi_CameraInfo camera = {};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &params;
    check(api.SetupCamera(&camera), "SetupCamera");
    for (size_t i = 0; i < placements.size() && status == REMIXAPI_ERROR_CODE_SUCCESS; ++i) {
      remixapi_InstanceIdentityEXT identity = {};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = i + 1;
      identity.classification = 0;
      identity.rasterVisible = true;
      remixapi_InstanceInfo instance = {};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &identity;
      instance.mesh = placements[i].mesh;
      instance.transform = placements[i].transform;
      instance.doubleSided = placements[i].doubleSided;
      check(api.DrawInstance(&instance), "DrawInstance");
    }
    for (unsigned i = 0; i < options.churn && status == REMIXAPI_ERROR_CODE_SUCCESS; ++i) {
      churnMesh(i, frame);
      remixapi_InstanceIdentityEXT identity = {};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 0x100000 + i;
      identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      identity.rasterVisible = true;
      remixapi_InstanceInfo instance = {};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &identity;
      instance.mesh = churnMeshes[i];
      for (int axis = 0; axis < 3; ++axis)
        instance.transform.matrix[axis][axis] = 1;
      instance.transform.matrix[0][3] = float(i % 16) * 2.5f - 20;
      instance.transform.matrix[1][3] = 1;
      instance.transform.matrix[2][3] = 6 + float(i / 16) * 2.5f;
      instance.doubleSided = 1;
      check(api.DrawInstance(&instance), "DrawInstance churn");
    }
    const auto presentStart = Clock::now();
    check(api.Present(nullptr), "Present");
    const auto now = Clock::now();
    remixapi_TracingStatisticsEXT trace = {};
    trace.structSize = sizeof(trace);
    remixapi_FramePerformanceEXT perf = {};
    perf.structSize = sizeof(perf);
    if (status == REMIXAPI_ERROR_CODE_SUCCESS && (options.stats || !ready)) {
      check(framePerformance(&perf), "GetFramePerformanceEXT");
      if (tracing)
        check(tracingStatistics(&trace), "GetTracingStatisticsEXT");
    }
    if (frame == 0)
      firstFrameMs = milliseconds(now - previous);
    else
      longestMs = std::max(longestMs, milliseconds(now - previous));
    if (!ready && (!tracing || trace.gpuTimingsAvailable)) {
      ready = frame + 1;
      readyMs = milliseconds(now - begin);
    } else if (!ready && milliseconds(now - begin) > 900000) {
      check(REMIXAPI_ERROR_CODE_GENERAL_FAILURE, "waiting for the backend");
    }
    if (ready && frame >= ready + warmup) {
      ++measured;
      presents.push_back(milliseconds(now - presentStart));
      framesMs.push_back(milliseconds(now - previous));
      const double values[] = {perf.sceneMilliseconds, perf.recordMilliseconds, perf.geometryMilliseconds,
                               perf.uploadMilliseconds, perf.accelerationMilliseconds, perf.fenceMilliseconds,
                               perf.acquireMilliseconds, perf.submitMilliseconds, perf.queuePresentMilliseconds};
      for (unsigned i = 0; i < 9; ++i)
        phases[i].push_back(values[i]);
      static remixapi_GpuPassTimingsEXT passes;
      passes = {};
      passes.structSize = sizeof(passes);
      if (passTimings && passTimings(&passes) == REMIXAPI_ERROR_CODE_SUCCESS && passes.available &&
          passes.frame != lastPassFrame) {
        lastPassFrame = passes.frame;
        gpuFrame.push_back(passes.frameMilliseconds);
        double upload = 0, assembly = 0;
        for (uint32_t i = 0; i < passes.passCount && i < REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT; ++i) {
          if (std::strcmp(passes.passes[i].name, "Scene uploads (geometry, textures)") == 0)
            upload += passes.passes[i].selfMilliseconds;
          if (std::strcmp(passes.passes[i].name, "Delta vertex assembly") == 0)
            assembly += passes.passes[i].selfMilliseconds;
        }
        uploadPass.push_back(upload);
        assemblyPass.push_back(assembly);
      }
      if (options.stats && tracing && trace.gpuTimingsAvailable) {
        gpuTrace.push_back(trace.gpuTraceMilliseconds);
        gpuBuild.push_back(trace.gpuBuildMilliseconds);
      }
    }
    previous = now;
  }
  const auto shutdown = remixapi_lib_shutdownAndUnloadRemixDll(&api, library);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS || shutdown != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "Remix runtime failed: 0x%x, shutdown: 0x%x\n", unsigned(status), unsigned(shutdown));
    return 1;
  }
  const auto present = summarize(presents), frame = summarize(framesMs);
  std::printf("instances=%zu textures=%u size=%u format=%s texture_mb=%.1f\n", placements.size(), options.textures + 1,
              options.size, options.compressed ? "bc" : "rgba", textureBytes / 1048576.0);
  std::printf("create_textures_ms=%.2f first_frame_ms=%.2f longest_frame_ms=%.2f backend_ready_ms=%.0f\n", textureMs,
              firstFrameMs, longestMs, readyMs);
  std::printf("present_ms mean=%.3f p50=%.3f p95=%.3f\n", present.mean, present.p50, present.p95);
  std::printf("frame_ms mean=%.3f p50=%.3f p95=%.3f fps=%.1f\n", frame.mean, frame.p50, frame.p95,
              frame.mean > 0 ? 1000 / frame.mean : 0);
  std::printf("present_phases_ms");
  for (unsigned i = 0; i < 9; ++i)
    std::printf(" %s=%.3f", phaseNames[i], summarize(phases[i]).mean);
  std::printf("\n");
  if (tracing)
    std::printf("gpu_ms trace=%.3f build=%.3f\n", summarize(gpuTrace).mean, summarize(gpuBuild).mean);
  if (!gpuFrame.empty())
    std::printf("gpu_pass_ms frame=%.3f uploads=%.3f assembly=%.3f (churn=%u x %zu triangles)\n",
                summarize(gpuFrame).mean, summarize(uploadPass).mean, summarize(assemblyPass).mean, options.churn,
                churnIndices.size() / 3);
  return 0;
}
