// Fallout 4-sized frame benchmark for the CPU cost of remixapi_Present.
// Mimics a large host's per-frame call pattern: ~12k static scene
// instances over a few thousand meshes and materials, ~40 CPU-skinned actors
// drawn every frame with stable dynamic identities, first-person skinned
// meshes, per-frame sphere lights, a sun, world + view-model cameras and a
// full-screen CPU overlay. Path tracing with raster primary visibility.
//
// Needs a display and a ray-tracing Vulkan device. Runs only when
// REMIX_BENCH_SCENE is set:
//   meson test -C <build> --benchmark "Scene frame CPU cost" (sets it), or run
//   remix-scene-frame-benchmark.exe directly with REMIX_BENCH_SCENE=1.
// Optional environment (defaults in brackets):
//   REMIX_BENCH_FRAMES [300] measured frames, REMIX_BENCH_WARMUP [120]
//   REMIX_BENCH_WIDTH/HEIGHT [1920x1080] window size
//   REMIX_BENCH_STATIC [12000] retained instances, REMIX_BENCH_MESHES [4000]
//   REMIX_BENCH_MATERIALS [1500], REMIX_BENCH_ACTORS [40], REMIX_BENCH_LIGHTS [64]
//   REMIX_BENCH_EMISSIVE [300] retained emissive fixtures (18 triangles each)
//   REMIX_BENCH_OVERLAY [1] full-screen DrawScreenOverlay every frame; 2 also
//     redraws a compass strip each frame (a changing HUD), 0 none
//   REMIX_BENCH_STABLE_IDS [1] InstanceIdentityEXT on per-frame draws
//   REMIX_BENCH_CONFIG "key=value;key=value" extra SetConfigVariable calls
//   REMIX_BENCH_DIGEST <file> write an address-free digest of every measured
//     frame's GPU inputs (output-identity checks between builds); the scene
//     animates by frame index, so equal builds give equal files.
#include "api/exports.h"
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <string>
#include <vector>

namespace {
  void ok(remixapi_ErrorCode code, const char *what) {
    if (code) {
      std::fprintf(stderr, "%s failed: %x\n", what, unsigned(code));
      std::exit(1);
    }
  }
  uint32_t env(const char *name, uint32_t fallback) {
    const char *value = std::getenv(name);
    return value && *value ? uint32_t(std::strtoul(value, nullptr, 10)) : fallback;
  }
  struct Random {
    uint64_t state;
    uint32_t next() {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return uint32_t(state >> 33);
    }
    float unit() { return float(next() & 0xffffff) / float(0x1000000); }
    uint32_t range(uint32_t low, uint32_t high) { return low + next() % (high - low + 1); }
  };
  std::filesystem::path path(uint64_t hash) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(hash));
    return text;
  }
  remixapi_Transform translation(float x, float y, float z, float yaw = 0, float scale = 1) {
    remixapi_Transform t{};
    const float c = std::cos(yaw) * scale, s = std::sin(yaw) * scale;
    t.matrix[0][0] = c;
    t.matrix[0][1] = -s;
    t.matrix[1][0] = s;
    t.matrix[1][1] = c;
    t.matrix[2][2] = scale;
    t.matrix[0][3] = x;
    t.matrix[1][3] = y;
    t.matrix[2][3] = z;
    return t;
  }
  // A bumpy grid patch: w x h vertices in the XY plane.
  void patch(Random &random, uint32_t w, uint32_t h, float size, std::vector<remixapi_HardcodedVertex> &vertices,
             std::vector<uint32_t> &indices) {
    vertices.clear();
    indices.clear();
    const float bump = random.unit() * .2f;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        remixapi_HardcodedVertex v{};
        const float u = float(x) / float(w - 1), t = float(y) / float(h - 1);
        v.position[0] = (u - .5f) * size;
        v.position[1] = (t - .5f) * size;
        v.position[2] = bump * std::sin(u * 6.28f) * std::cos(t * 6.28f) * size;
        v.normal[2] = 1;
        v.texcoord[0] = u;
        v.texcoord[1] = t;
        v.color = 0xffffffff;
        vertices.push_back(v);
      }
    for (uint32_t y = 0; y + 1 < h; ++y)
      for (uint32_t x = 0; x + 1 < w; ++x) {
        const uint32_t a = y * w + x, b = a + 1, c = a + w, d = c + 1;
        indices.insert(indices.end(), {a, b, d, a, d, c});
      }
  }
  struct Skin {
    remixapi_MeshHandle mesh{};
    uint32_t vertices = 0;
  };
  // A skinned "body": surfaces x (w x h) patches, 4 weighted bones per vertex.
  Skin skinnedMesh(remixapi_Interface &api, Random &random, uint64_t hash, uint32_t surfaces, uint32_t vertices,
                   uint32_t bones, const std::vector<remixapi_MaterialHandle> &materials) {
    std::vector<std::vector<remixapi_HardcodedVertex>> vertexData(surfaces);
    std::vector<std::vector<uint32_t>> indexData(surfaces), boneData(surfaces);
    std::vector<std::vector<float>> weightData(surfaces);
    std::vector<remixapi_MeshInfoSurfaceTriangles> info(surfaces);
    Skin skin;
    for (uint32_t s = 0; s < surfaces; ++s) {
      const uint32_t side = std::max(4u, uint32_t(std::sqrt(float(vertices / surfaces))));
      patch(random, side, side, 1.5f, vertexData[s], indexData[s]);
      for (auto &v : vertexData[s]) {
        // Wrap the patch into a cylinder segment standing on the ground.
        const float angle = v.position[0] * 2.f, height = v.position[1] + .75f + float(s) * .4f;
        v.position[0] = std::cos(angle) * .3f;
        v.position[1] = std::sin(angle) * .3f;
        v.position[2] = height;
        v.normal[0] = std::cos(angle);
        v.normal[1] = std::sin(angle);
        v.normal[2] = 0;
        uint32_t first = std::min(bones - 4, uint32_t(height / 3.f * float(bones - 4)));
        for (uint32_t b = 0; b < 4; ++b) {
          boneData[s].push_back(first + b);
          weightData[s].push_back(b == 0 ? .55f : .15f);
        }
      }
      skin.vertices += uint32_t(vertexData[s].size());
      auto &surface = info[s];
      surface.vertices_values = vertexData[s].data();
      surface.vertices_count = vertexData[s].size();
      surface.indices_values = indexData[s].data();
      surface.indices_count = indexData[s].size();
      surface.skinning_hasvalue = 1;
      surface.skinning_value.bonesPerVertex = 4;
      surface.skinning_value.blendWeights_values = weightData[s].data();
      surface.skinning_value.blendWeights_count = uint32_t(weightData[s].size());
      surface.skinning_value.blendIndices_values = boneData[s].data();
      surface.skinning_value.blendIndices_count = uint32_t(boneData[s].size());
      surface.material = materials[random.next() % materials.size()];
    }
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = hash;
    mesh.surfaces_values = info.data();
    mesh.surfaces_count = surfaces;
    ok(api.CreateMesh(&mesh, &skin.mesh), "CreateMesh (skinned)");
    return skin;
  }
  void pose(uint32_t frame, uint32_t actor, uint32_t bones, std::vector<remixapi_Transform> &out) {
    out.resize(bones);
    for (uint32_t b = 0; b < bones; ++b) {
      const float angle = .15f * std::sin(float(frame) * .07f + float(actor) * .9f + float(b) * .3f);
      out[b] = translation(.02f * std::sin(float(frame) * .05f + float(b)), 0, 0, angle);
    }
  }
  struct Stats {
    std::vector<double> samples;
    void add(double v) { samples.push_back(v); }
    double mean() const {
      double sum = 0;
      for (double v : samples)
        sum += v;
      return samples.empty() ? 0 : sum / double(samples.size());
    }
    double median() const {
      if (samples.empty())
        return 0;
      auto copy = samples;
      std::nth_element(copy.begin(), copy.begin() + copy.size() / 2, copy.end());
      return copy[copy.size() / 2];
    }
  };
} // namespace

int main() {
  if (!std::getenv("REMIX_BENCH_SCENE")) {
    std::puts("Scene frame benchmark skipped (set REMIX_BENCH_SCENE=1)");
    return 77;
  }
  const uint32_t frames = env("REMIX_BENCH_FRAMES", 300), warmup = env("REMIX_BENCH_WARMUP", 120);
  const uint32_t width = env("REMIX_BENCH_WIDTH", 1920), height = env("REMIX_BENCH_HEIGHT", 1080);
  const uint32_t staticCount = env("REMIX_BENCH_STATIC", 12000), meshCount = env("REMIX_BENCH_MESHES", 4000);
  const uint32_t materialCount = env("REMIX_BENCH_MATERIALS", 1500), actorCount = env("REMIX_BENCH_ACTORS", 40);
  const uint32_t lightCount = env("REMIX_BENCH_LIGHTS", 64);
  const uint32_t emissiveCount = env("REMIX_BENCH_EMISSIVE", 300);
  const uint32_t overlayMode = env("REMIX_BENCH_OVERLAY", 1);
  const bool overlay = overlayMode != 0, stableIds = env("REMIX_BENCH_STABLE_IDS", 1) != 0;
  const char *digestPath = std::getenv("REMIX_BENCH_DIGEST");
  if (digestPath) {
#if defined(_WIN32)
    _putenv_s("REMIX_FRAME_DIGEST", "1");
#else
    setenv("REMIX_FRAME_DIGEST", "1", 1);
#endif
  }

  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api), "InitializeLibrary");
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = width;
  startup.windowHeight = height;
#endif
  ok(api.Startup(&startup), "Startup");
  SDL_SetWindowSize(remix::api::g_runtime.window().handle(), int(width), int(height));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps), "GetBackendCapabilitiesEXT");
  if (!(caps.supportedBackends & 2)) {
    std::printf("Path tracing unavailable: %s\n", caps.unavailableReason);
    ok(api.Shutdown(), "Shutdown");
    return 77;
  }
  auto set = [&](const std::string &key, const std::string &value) {
    ok(api.SetConfigVariable(key.c_str(), value.c_str()), key.c_str());
  };
  set("rtx.native.backend", "pathtrace");
  set("rtx.native.vsync", "0");
  set("rtx.native.textures.keepCompressed", "1");
  if (const char *extra = std::getenv("REMIX_BENCH_CONFIG")) {
    std::string list = extra;
    size_t start = 0;
    while (start < list.size()) {
      size_t end = list.find(';', start);
      if (end == std::string::npos)
        end = list.size();
      const auto item = list.substr(start, end - start);
      const auto equals = item.find('=');
      if (equals != std::string::npos) {
        std::printf("config %s\n", item.c_str());
        set(item.substr(0, equals), item.substr(equals + 1));
      }
      start = end + 1;
    }
  }

  Random random{12345};
  // Textures: BC1 albedo (kept compressed, as the plugin uploads), RGBA8 maps.
  const uint32_t albedoCount = std::max(1u, materialCount), mapCount = std::max(1u, materialCount / 3);
  auto texture = [&](uint64_t hash, remixapi_Format format, uint32_t size, const void *data, uint64_t bytes) {
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = hash;
    info.width = info.height = size;
    info.depth = info.mipLevels = 1;
    info.format = format;
    info.data = data;
    info.dataSize = bytes;
    remixapi_TextureHandle handle;
    ok(api.CreateTexture(&info, &handle), "CreateTexture");
  };
  std::vector<uint8_t> bc1(8 * 16 * 16), rgba(4 * 32 * 32);
  for (uint32_t i = 0; i < albedoCount; ++i) {
    for (auto &byte : bc1)
      byte = uint8_t(random.next());
    texture(0x100000 + i, REMIXAPI_FORMAT_BC1_RGB_SRGB, 64, bc1.data(), bc1.size());
  }
  for (uint32_t i = 0; i < 2 * mapCount; ++i) {
    for (size_t p = 0; p < rgba.size(); p += 4) {
      rgba[p] = uint8_t(96 + random.next() % 64);
      rgba[p + 1] = uint8_t(96 + random.next() % 64);
      rgba[p + 2] = 255;
      rgba[p + 3] = 255;
    }
    texture(0x200000 + i, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 32, rgba.data(), rgba.size());
  }
  std::vector<std::filesystem::path> paths(albedoCount + 2 * mapCount);
  for (uint32_t i = 0; i < albedoCount; ++i)
    paths[i] = path(0x100000 + i);
  for (uint32_t i = 0; i < 2 * mapCount; ++i)
    paths[albedoCount + i] = path(0x200000 + i);

  std::vector<remixapi_MaterialHandle> materials(materialCount);
  for (uint32_t i = 0; i < materialCount; ++i) {
    remixapi_MaterialInfoPbrEXT pbr{};
    pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    pbr.enabled = 1;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &pbr;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = .3f + .6f * random.unit();
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = 0x300000 + i;
    info.albedoTexture = paths[i % albedoCount].c_str();
    info.filterMode = 1;
    info.wrapModeU = info.wrapModeV = 1;
    if (i % 3 == 0) {
      info.normalTexture = paths[albedoCount + (i / 3) % mapCount].c_str();
      opaque.roughnessTexture = paths[albedoCount + mapCount + (i / 3) % mapCount].c_str();
    }
    ok(api.CreateMaterial(&info, &materials[i]), "CreateMaterial");
  }

  // Static meshes: 1-2 surfaces of 48..480 vertices.
  std::vector<remixapi_MeshHandle> meshes(meshCount);
  std::vector<remixapi_HardcodedVertex> vertices[2];
  std::vector<uint32_t> indices[2];
  uint64_t staticVertices = 0;
  for (uint32_t i = 0; i < meshCount; ++i) {
    const uint32_t surfaces = random.next() % 5 == 0 ? 2 : 1;
    remixapi_MeshInfoSurfaceTriangles info[2]{};
    for (uint32_t s = 0; s < surfaces; ++s) {
      const uint32_t w = random.range(6, 24), h = random.range(8, 20);
      patch(random, w, h, 1 + 4 * random.unit(), vertices[s], indices[s]);
      staticVertices += vertices[s].size();
      info[s].vertices_values = vertices[s].data();
      info[s].vertices_count = vertices[s].size();
      info[s].indices_values = indices[s].data();
      info[s].indices_count = indices[s].size();
      info[s].material = materials[random.next() % materialCount];
    }
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = 0x400000 + i;
    mesh.surfaces_values = info;
    mesh.surfaces_count = surfaces;
    ok(api.CreateMesh(&mesh, &meshes[i]), "CreateMesh");
  }

  // Retained static placements over a 600 x 600 unit area.
  for (uint32_t i = 0; i < staticCount; ++i) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 1 + i;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &identity;
    instance.mesh = meshes[random.next() % meshCount];
    instance.transform = translation(600 * random.unit() - 300, 600 * random.unit() - 300, 3 * random.unit(),
                                     6.28f * random.unit(), .5f + random.unit());
    uint64_t handle = 0;
    ok(remixapi_AddSceneInstancesEXT(&instance, 1, &handle), "AddSceneInstancesEXT");
  }

  // Emissive fixtures (lamps, signs, screens): 10 emissive materials, half
  // with an emission texture, on 20 small retained meshes.
  if (emissiveCount) {
    std::vector<remixapi_MaterialHandle> glowing(10);
    std::vector<std::filesystem::path> glowPaths(5);
    for (uint32_t i = 0; i < 5; ++i) {
      for (size_t p = 0; p < rgba.size(); p += 4) {
        rgba[p] = uint8_t(128 + random.next() % 128);
        rgba[p + 1] = uint8_t(96 + random.next() % 128);
        rgba[p + 2] = uint8_t(64 + random.next() % 64);
        rgba[p + 3] = 255;
      }
      texture(0x800000 + i, REMIXAPI_FORMAT_R8G8B8A8_SRGB, 32, rgba.data(), rgba.size());
      glowPaths[i] = path(0x800000 + i);
    }
    for (uint32_t i = 0; i < glowing.size(); ++i) {
      remixapi_MaterialInfoOpaqueEXT opaque{};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.albedoConstant = {1, 1, 1};
      opaque.opacityConstant = 1;
      opaque.roughnessConstant = .5f;
      opaque.alphaTestType = 7;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &opaque;
      info.hash = 0x810000 + i;
      info.albedoTexture = paths[i % albedoCount].c_str();
      if (i < 5)
        info.emissiveTexture = glowPaths[i].c_str();
      info.emissiveIntensity = 4;
      info.emissiveColorConstant = {1, .8f, .6f};
      ok(api.CreateMaterial(&info, &glowing[i]), "CreateMaterial (emissive)");
    }
    std::vector<remixapi_MeshHandle> lamps(20);
    for (uint32_t i = 0; i < lamps.size(); ++i) {
      patch(random, 4, 4, .5f, vertices[0], indices[0]);
      remixapi_MeshInfoSurfaceTriangles info{};
      info.vertices_values = vertices[0].data();
      info.vertices_count = vertices[0].size();
      info.indices_values = indices[0].data();
      info.indices_count = indices[0].size();
      info.material = glowing[i % glowing.size()];
      remixapi_MeshInfo mesh{};
      mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      mesh.hash = 0x820000 + i;
      mesh.surfaces_values = &info;
      mesh.surfaces_count = 1;
      ok(api.CreateMesh(&mesh, &lamps[i]), "CreateMesh (lamp)");
    }
    for (uint32_t i = 0; i < emissiveCount; ++i) {
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 100000 + i;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &identity;
      instance.mesh = lamps[i % lamps.size()];
      instance.transform = translation(200 * random.unit() - 100, 200 * random.unit() - 100, 2 + 3 * random.unit(),
                                       6.28f * random.unit());
      uint64_t handle = 0;
      ok(remixapi_AddSceneInstancesEXT(&instance, 1, &handle), "AddSceneInstancesEXT (lamp)");
    }
  }

  // Skinned actors (8 shared bodies, one skin per actor) and first-person meshes.
  constexpr uint32_t boneCount = 60;
  std::vector<Skin> bodies;
  uint64_t actorVertices = 0;
  for (uint32_t i = 0; i < 8; ++i)
    bodies.push_back(skinnedMesh(api, random, 0x500000 + i, 4, 3000 + 700 * i, boneCount, materials));
  std::vector<Skin> firstPerson;
  for (uint32_t i = 0; i < 2; ++i)
    firstPerson.push_back(skinnedMesh(api, random, 0x600000 + i, 3, 4000, boneCount, materials));
  struct Actor {
    uint32_t body;
    float x, y, yaw;
  };
  std::vector<Actor> actors;
  for (uint32_t i = 0; i < actorCount; ++i) {
    actors.push_back({i % 8, 40 * random.unit() - 20, 30 + 40 * random.unit(), 6.28f * random.unit()});
    actorVertices += bodies[i % 8].vertices;
  }

  // Sphere lights (drawn every frame) and the sun.
  std::vector<remixapi_LightHandle> lights(lightCount);
  for (uint32_t i = 0; i < lightCount; ++i) {
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {80 * random.unit() - 40, 20 + 80 * random.unit(), 2 + 3 * random.unit()};
    sphere.radius = .1f;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &sphere;
    light.hash = 0x700000 + i;
    light.radiance = {20, 16, 12};
    ok(api.CreateLight(&light, &lights[i]), "CreateLight");
  }
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {.3f, .4f, -.8f};
  distant.angularDiameterDegrees = .5f;
  remixapi_LightInfo sunInfo{};
  sunInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  sunInfo.pNext = &distant;
  sunInfo.hash = 0x7fffff;
  sunInfo.radiance = {3, 3, 3};
  remixapi_LightHandle sun;
  ok(api.CreateLight(&sunInfo, &sun), "CreateLight (sun)");

  // Mostly transparent HUD with a few opaque bars, like a captured Scaleform layer.
  std::vector<uint8_t> hud(size_t(width) * height * 4, 0);
  for (uint32_t y = height - height / 10; y < height - height / 20; ++y)
    for (uint32_t x = width / 20; x < width / 4; ++x) {
      auto *p = &hud[(size_t(y) * width + x) * 4];
      p[0] = 40;
      p[1] = 230;
      p[2] = 90;
      p[3] = 200;
    }

  std::vector<remixapi_Transform> bones;
  double submitMilliseconds = 0;
  auto submit = [&](uint32_t frame) {
    if (overlayMode == 2) {
      // Compass strip: a marker that moves every frame.
      for (uint32_t y = height / 40; y < height / 20; ++y)
        for (uint32_t x = width / 3; x < 2 * width / 3; ++x) {
          auto *p = &hud[(size_t(y) * width + x) * 4];
          const bool marker = (x + frame * 3) % 97 < 4;
          p[0] = p[1] = p[2] = marker ? 255 : 30;
          p[3] = marker ? 255 : 90;
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    ok(api.PumpEvents(), "PumpEvents");
    remixapi_CameraInfoParameterizedEXT parameters{};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    const float t = float(frame) * .01f;
    parameters.position = {4 * std::sin(t), -10 + 2 * t, 1.7f};
    parameters.forward = {std::sin(t * .5f) * .3f, 1, -.05f};
    parameters.up = {0, 0, 1};
    parameters.right = {1, -std::sin(t * .5f) * .3f, 0};
    parameters.fovYInDegrees = 70;
    parameters.aspect = float(width) / float(height);
    parameters.nearPlane = .1f;
    parameters.farPlane = 5000;
    remixapi_CameraInfo camera{};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
    ok(api.SetupCamera(&camera), "SetupCamera");
    camera.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
    parameters.fovYInDegrees = 80;
    ok(api.SetupCamera(&camera), "SetupCamera (view model)");
    ok(api.DrawLightInstance(sun), "DrawLightInstance (sun)");
    for (auto light : lights)
      ok(api.DrawLightInstance(light), "DrawLightInstance");
    ok(remixapi_DrawSceneInstancesEXT(nullptr), "DrawSceneInstancesEXT");
    auto skinned = [&](remixapi_MeshHandle mesh, uint64_t id, uint32_t actor, const remixapi_Transform &transform,
                       uint32_t category) {
      pose(frame, actor, boneCount, bones);
      remixapi_InstanceInfoBoneTransformsEXT boneInfo{};
      boneInfo.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
      boneInfo.boneTransforms_values = bones.data();
      boneInfo.boneTransforms_count = boneCount;
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = id;
      identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      identity.rasterVisible = 1;
      if (stableIds)
        boneInfo.pNext = &identity;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &boneInfo;
      instance.categoryFlags = category;
      instance.mesh = mesh;
      instance.transform = transform;
      ok(api.DrawInstance(&instance), "DrawInstance");
    };
    for (uint32_t a = 0; a < actors.size(); ++a) {
      const auto &actor = actors[a];
      const float walk = std::sin(float(frame) * .01f + float(a)) * 2;
      skinned(bodies[actor.body].mesh, 0x10000000ull + a, a, translation(actor.x + walk, actor.y, 0, actor.yaw), 0);
    }
    for (uint32_t i = 0; i < firstPerson.size(); ++i)
      skinned(firstPerson[i].mesh, 0x20000000ull + i, 100 + i, translation(.2f, .5f, -.3f),
              REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL);
    if (overlay)
      ok(api.DrawScreenOverlay(hud.data(), width, height, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 1), "DrawScreenOverlay");
    submitMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    ok(api.Present(nullptr), "Present");
  };

  std::printf("scene: %u retained over %u meshes (%llu vertices), %u materials, %u actors (%llu skinned "
              "vertices), %u lights, %u emissive fixtures, overlay %s, stable ids %s, %ux%u\n",
              staticCount, meshCount, (unsigned long long)staticVertices, materialCount, actorCount,
              (unsigned long long)actorVertices, lightCount, emissiveCount,
              overlayMode == 2 ? "changing"
              : overlay        ? "static"
                               : "off",
              stableIds ? "on" : "off", width, height);
  std::fflush(stdout);
  // Pipeline preparation repeats frame 0; afterwards the scene animates by
  // frame index, so digests of equal frame numbers are comparable.
  uint32_t frame = 0;
  auto prepared = remix::test::preparedFrame([&] { submit(frame); });
  prepared();
  for (uint32_t i = 0; i < warmup; ++i)
    prepared(), ++frame;

  std::FILE *digest = digestPath ? std::fopen(digestPath, "w") : nullptr;
  Stats present, scene, record, fence, acquire, submitTime, queuePresent, geometry, alloc, upload, accel, calls;
  constexpr size_t phaseCount = 23;
  Stats phases[phaseCount];
  uint64_t conversions = 0, uploadBytes = 0, surfaceCount = 0, primaryCount = 0, rasterCount = 0, textureCount = 0,
           skinnedCount = 0, rasterBytes = 0, measured = 0;
  const auto wall = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < frames; ++i) {
    submitMilliseconds = 0;
    submit(frame);
    remixapi_FramePerformanceEXT p{};
    p.structSize = sizeof(p);
    ok(remixapi_GetFramePerformanceEXT(&p), "GetFramePerformanceEXT");
    remixapi_FrameBreakdownEXT b{};
    b.structSize = sizeof(b);
    ok(remixapi_GetFrameBreakdownEXT(&b), "GetFrameBreakdownEXT");
    if (digest)
      std::fprintf(digest, "%u %016llx\n", frame, (unsigned long long)remix::api::g_runtime.frameDigest());
    ++frame;
    if (!p.presented || !p.recordMilliseconds)
      continue;
    ++measured;
    present.add(p.presentMilliseconds);
    scene.add(p.sceneMilliseconds);
    record.add(p.recordMilliseconds);
    fence.add(p.fenceMilliseconds);
    acquire.add(p.acquireMilliseconds);
    submitTime.add(p.submitMilliseconds);
    queuePresent.add(p.queuePresentMilliseconds);
    geometry.add(p.geometryMilliseconds);
    alloc.add(p.allocationMilliseconds);
    upload.add(p.uploadMilliseconds);
    accel.add(p.accelerationMilliseconds);
    calls.add(submitMilliseconds);
    const double values[phaseCount] = {b.rasterPrepareMilliseconds,
                                       b.rasterSkinningMilliseconds,
                                       b.traceBeginMilliseconds,
                                       b.traceSetupMilliseconds,
                                       b.traceSurfacesMilliseconds,
                                       b.materialMilliseconds,
                                       b.temporalMilliseconds,
                                       b.retireMilliseconds,
                                       b.traceAccelerationMilliseconds,
                                       b.tableMilliseconds,
                                       b.lightingMilliseconds,
                                       b.sharcMilliseconds,
                                       b.signatureMilliseconds,
                                       b.descriptorMilliseconds,
                                       b.primaryMilliseconds,
                                       b.passesMilliseconds,
                                       b.postMilliseconds,
                                       b.compositeMilliseconds,
                                       b.uiMilliseconds,
                                       b.snapshotMilliseconds,
                                       b.otherMilliseconds,
                                       b.presentSetupMilliseconds,
                                       b.presentReleaseMilliseconds};
    for (size_t k = 0; k < phaseCount; ++k)
      phases[k].add(values[k]);
    conversions += p.geometryConversions;
    uploadBytes += p.uploadBytes;
    surfaceCount = b.traceSurfaces;
    primaryCount = b.primaryDraws;
    rasterCount = b.rasterDraws;
    textureCount = b.textureDescriptors;
    skinnedCount = b.rasterSkinnedVertices;
    rasterBytes += b.rasterUploadBytes;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall).count();
  if (digest)
    std::fclose(digest);
  auto line = [](const char *name, const Stats &s) {
    std::printf("  %-22s mean %8.3f  median %8.3f ms\n", name, s.mean(), s.median());
  };
  std::printf("measured %llu of %u frames, %.1f FPS wall clock\n", (unsigned long long)measured, frames,
              double(frames) / seconds);
  std::puts("FramePerformanceEXT:");
  line("present", present);
  line("scene", scene);
  line("fence", fence);
  line("acquire", acquire);
  line("record", record);
  line("  geometry", geometry);
  line("  allocation", alloc);
  line("  upload", upload);
  line("  acceleration", accel);
  line("submit", submitTime);
  line("queue present", queuePresent);
  line("API calls before Present", calls);
  std::puts("FrameBreakdownEXT (within record, then the rest of present):");
  const char *names[phaseCount] = {"raster prepare",  "  raster skinning", "tracer begin",     "tracer setup",
                                   "trace surfaces",  "  materials",       "  temporal",       "retire",
                                   "acceleration",    "surface table",     "lighting",         "sharc",
                                   "scene signature", "descriptors",       "primary g-buffer", "passes",
                                   "post-processing", "composite",         "developer ui",     "snapshot",
                                   "other",           "(present) setup",   "(present) release"};
  for (size_t k = 0; k < phaseCount; ++k)
    line(names[k], phases[k]);
  const double n = double(std::max<uint64_t>(measured, 1));
  std::printf("per frame: %.1f conversions, %.2f MiB tracer uploads, %.2f MiB raster uploads, %llu trace "
              "surfaces, %llu primary draws, %llu raster draws, %llu textures, %llu raster skinned vertices\n",
              double(conversions) / n, double(uploadBytes) / n / (1024 * 1024), double(rasterBytes) / n / (1024 * 1024),
              (unsigned long long)surfaceCount, (unsigned long long)primaryCount, (unsigned long long)rasterCount,
              (unsigned long long)textureCount, (unsigned long long)skinnedCount);
  ok(api.Shutdown(), "Shutdown");
  return 0;
}
