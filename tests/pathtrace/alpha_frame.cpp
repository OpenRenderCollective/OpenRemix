// GPU cost of alpha, by pass (rtx.native.gpuPassTimers): a lit room with
// blended particles, soft smoke and alpha-tested foliage in front of the camera.
//
// The blood is drawn as UnityRTX draws ULTRAKILL's: sprays of small
// camera-facing drops (a 16x16 point-sampled texture whose alpha is 0 or 1,
// red vertex colour) with short stretched ribbon trails, and fewer
// translucent drops (alpha 0.78). Every frame each kind is one world-space
// mesh created again under the same hash (CreateMesh replaces it), drawn as a
// particle (REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE) with alpha-over blending
// (remixapi_InstanceInfoBlendEXT: texel times vertex colour) and a material
// that blends (blendType 1) and alpha-tests at 2/255.
//
// The stains are ULTRAKILL's blood on floors and walls as UnityRTX draws them
// (UltrakillBloodStains): one quad per stain lifted 1 cm off its surface,
// mapped to a cell of the game's 160x32 splatter atlas, all in one mesh drawn
// as multiplicative paint (DST_COLOR * ZERO, a dynamic decal) with an alpha
// test at 0.5 and a wet finish (remixapi_MaterialInfoPaintFinishEXT).
//
// Needs a display and a ray-tracing Vulkan device. Runs only when
// REMIX_BENCH_ALPHA is set, to a comma-separated list of what to draw:
// blood, stains, smoke, foliage (or none: the room alone).
// Optional environment (defaults in brackets):
//   REMIX_BENCH_FRAMES [300] measured frames, REMIX_BENCH_WARMUP [200]
//   REMIX_BENCH_WIDTH/HEIGHT [1920x1080] window size
//   REMIX_BENCH_SPRAYS [6] blood sprays, REMIX_BENCH_DROPS [75] drops a spray,
//   REMIX_BENCH_FAINT [20] translucent drops a spray
//   REMIX_BENCH_STAINS [800] blood stains, REMIX_BENCH_STAIN_EVERY [0] frames
//     between a new stain replacing the oldest (its mesh created again; 0: never)
//   REMIX_BENCH_PAINT_FINISH [1] 0: the stains tint only, without their wet finish
//   REMIX_BENCH_PAINT_ROUGHNESS [15] the finish's roughness, percent
//   REMIX_BENCH_SMOKE [120] smoke billboards, REMIX_BENCH_LEAVES [6000] leaf quads
//   REMIX_BENCH_BLOOD_CUTOUT [0] 1: the drops alpha-tested, not blended
//   REMIX_BENCH_FREEZE [0] 1: the particles hold still
//   REMIX_BENCH_UPDATE [0] 1: particle meshes are updated in place (refits)
//   REMIX_BENCH_CONFIG_FILE a profile (key = value lines) applied first
//   REMIX_BENCH_CONFIG "key=value;key=value" applied after it
//   REMIX_BENCH_SCREENSHOT <file.png> the frame after the measured ones
// The scene animates by frame index at a fixed 60 Hz step, so runs compare.
#include "api/exports.h"
#include "api/state.h"
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
    float range(float low, float high) { return low + (high - low) * unit(); }
  };
  std::filesystem::path path(uint64_t hash) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(hash));
    return text;
  }
  struct V3 {
    float x, y, z;
  };
  V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
  V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
  V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
  V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
  V3 normalize(V3 a) {
    const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return l > 1e-8f ? a * (1 / l) : V3{0, 0, 1};
  }
  // D3DCOLOR (0xAARRGGBB), as hosts pack vertex colours.
  uint32_t color(float r, float g, float b, float a) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.f, 1.f) * 255 + .5f); };
    return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
  }
  struct Batch {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    void clear() {
      vertices.clear();
      indices.clear();
    }
    void vertex(V3 p, V3 n, float u, float v, uint32_t c) {
      remixapi_HardcodedVertex out{};
      out.position[0] = p.x;
      out.position[1] = p.y;
      out.position[2] = p.z;
      out.normal[0] = n.x;
      out.normal[1] = n.y;
      out.normal[2] = n.z;
      out.texcoord[0] = u;
      out.texcoord[1] = v;
      out.color = c;
      vertices.push_back(out);
    }
    // A quad from a centre and two half-extent axes.
    void quad(V3 centre, V3 right, V3 up, uint32_t c) {
      const auto base = uint32_t(vertices.size());
      const V3 n = normalize(cross(right, up));
      vertex(centre - right - up, n, 0, 1, c);
      vertex(centre + right - up, n, 1, 1, c);
      vertex(centre + right + up, n, 1, 0, c);
      vertex(centre - right + up, n, 0, 0, c);
      indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
  };
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
  // ULTRAKILL's blooddrop and blooddroptrail (16x16, alpha 0 or 1, white).
  const char *dropMask[16] = {
      "................", "................", "......####......", "....########....",
      "...##########...", "...##########...", "..############..", "..############..",
      "..############..", "..############..", "...##########...", "...##########...",
      "....########....", "......####......", "................", "................"};
  const char *trailMask[16] = {
      "................", "................", "##########......", "############....",
      "#############...", "#############...", "##############..", "##############..",
      "##############..", "##############..", "#############...", "#############...",
      "############....", "##########......", "................", "................"};
  // ULTRAKILL's Bloodsplatter_Atlas: 5 splatters side by side, alpha 0 or 1;
  // a row's 160 texels as hex, the lowest bit leftmost.
  const char *splatterAtlas[32] = {
      "00f0000000000000000000000000000000000000", "1ff000000007003c00007ff80600000000700000",
      "3f8000e07f8f003c001ffffc0fff000000f80018", "300180e07fff0f3000fffffc0ffff70003f800f8",
      "000781e07fffff8003fffffc07fffff807ff01f0", "063f81e03fffff800ffffff803fffffc07ffc1f0",
      "07fc19f03ffffffc3ffffff0001ffffc3fffc1f0", "03fe19f03ffffffc3dffffe039fffff87fffe0e0",
      "01ff81f07ffffc0031ffffc63ffffff07c7fe000", "00c380f07ffff00003fffc061ffffff07c7ffc00",
      "01c060787ffffc0007fff0001ffffff8387ffe00", "0783e03c7ffffff007ffe0003ffffff8003fff00",
      "071fc00c7ffffff00fffc1c03ffffff8003ffff8", "07fe00003fe7fff01fff87c03ffffff8000ffffc",
      "03ff00603fc3fe001fff0fc61ffffff80007fffc", "03fff8600f8ff0001ffc3fce1ffc7bf80007fffc",
      "7001f80007cef0001ff87fcc3ff0f3f81e0ffffc", "7e00300007c0f0001ff07fcc7f81e7f83f0ffffc",
      "3fe0338007cee0001fe01ec07e038ff83f1ffffe", "1ff81f801f9e03c01fc01ec038031ffc3f1ffffe",
      "1fffff801c7c0ff01f801cc000003ffc1e1ffffe", "0ffff7001cfc0ff01f801ce00001fffc001ffffe",
      "07ffffe003f81ff81f807ce03003fffc001ffffe", "07fffff807fb9ffe0f00fc607807fffc001ffffe",
      "3ffffff80ff7dffe0003fc0078dfe7bc001ffffc", "3dfffff01ff7dffe0003000031ff0fbe003ffffc",
      "1dfffff03fe7cff00607003801f80f1e003ffff8", "0ffffffc3fe38ff00e0f007800c00f1e003ffffc",
      "0ffffffc3fe003c01f0e03e00000060c001ffffe", "0e7f18c03fc003803f0e03c0000000000000fffe",
      "007000000e0003803c0003800000000000003f3e", "000000000000000000000000000000000000001c",
  };
} // namespace

int main() {
  const char *what = std::getenv("REMIX_BENCH_ALPHA");
  if (!what) {
    std::puts("Alpha frame benchmark skipped (set REMIX_BENCH_ALPHA=blood,stains,smoke,foliage or none)");
    return 77;
  }
  const std::string cases = what;
  const bool blood = cases.find("blood") != std::string::npos, smoke = cases.find("smoke") != std::string::npos,
             foliage = cases.find("foliage") != std::string::npos, stains = cases.find("stains") != std::string::npos;
  const uint32_t frames = env("REMIX_BENCH_FRAMES", 300), warmup = env("REMIX_BENCH_WARMUP", 200);
  const uint32_t width = env("REMIX_BENCH_WIDTH", 1920), height = env("REMIX_BENCH_HEIGHT", 1080);
  const uint32_t sprays = blood ? env("REMIX_BENCH_SPRAYS", 6) : 0, drops = env("REMIX_BENCH_DROPS", 75);
  const uint32_t faintDrops = env("REMIX_BENCH_FAINT", 20);
  const uint32_t smokeCount = smoke ? env("REMIX_BENCH_SMOKE", 120) : 0;
  const uint32_t leafCount = foliage ? env("REMIX_BENCH_LEAVES", 6000) : 0;
  const uint32_t stainCount = stains ? env("REMIX_BENCH_STAINS", 800) : 0, stainEvery = env("REMIX_BENCH_STAIN_EVERY", 0);

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
  auto trim = [](std::string v) {
    const auto a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
  };
  set("rtx.native.backend", "pathtrace");
  if (const char *file = std::getenv("REMIX_BENCH_CONFIG_FILE")) {
    std::ifstream in(file);
    if (!in) {
      std::fprintf(stderr, "Cannot read %s\n", file);
      return 1;
    }
    uint32_t applied = 0, refused = 0;
    for (std::string line; std::getline(in, line);) {
      const auto equals = line.find('=');
      if (line.empty() || line[0] == '#' || equals == std::string::npos)
        continue;
      const auto key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
      if (key.rfind("rtx.", 0) != 0)
        continue;
      if (api.SetConfigVariable(key.c_str(), value.c_str()))
        ++refused;
      else
        ++applied;
    }
    std::printf("profile %s: %u settings applied, %u refused\n", file, applied, refused);
  }
  if (const char *extra = std::getenv("REMIX_BENCH_CONFIG")) {
    std::string list = extra;
    for (size_t start = 0; start < list.size();) {
      size_t end = list.find(';', start);
      if (end == std::string::npos)
        end = list.size();
      const auto item = list.substr(start, end - start);
      const auto equals = item.find('=');
      if (equals != std::string::npos) {
        std::printf("config %s\n", item.c_str());
        set(trim(item.substr(0, equals)), trim(item.substr(equals + 1)));
      }
      start = end + 1;
    }
  }
  set("rtx.native.vsync", "0");
  set("rtx.native.gpuPassTimers", "1");

  Random random{0xb100d};
  auto texture = [&](uint64_t hash, remixapi_Format format, uint32_t size, const std::vector<uint8_t> &data) {
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = hash;
    info.width = info.height = size;
    info.depth = info.mipLevels = 1;
    info.format = format;
    info.data = data.data();
    info.dataSize = data.size();
    remixapi_TextureHandle handle;
    ok(api.CreateTexture(&info, &handle), "CreateTexture");
    return path(hash);
  };
  // The room's albedo: grey noise; masks: white texels, alpha from the mask.
  std::vector<uint8_t> pixels(64 * 64 * 4);
  for (size_t p = 0; p < pixels.size(); p += 4) {
    const auto v = uint8_t(110 + random.next() % 50);
    pixels[p] = pixels[p + 1] = pixels[p + 2] = v;
    pixels[p + 3] = 255;
  }
  const auto roomPath = texture(0xa1000, REMIXAPI_FORMAT_R8G8B8A8_SRGB, 64, pixels);
  auto mask = [&](uint64_t hash, const char *const *rows) {
    std::vector<uint8_t> texels(16 * 16 * 4, 255);
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 16; ++x)
        texels[(y * 16 + x) * 4 + 3] = rows[y][x] == '#' ? 255 : 0;
    return texture(hash, REMIXAPI_FORMAT_R8G8B8A8_SRGB, 16, texels);
  };
  const auto dropPath = mask(0xa1001, dropMask), trailPath = mask(0xa1002, trailMask);
  // Smoke: a soft disc of noisy, low coverage.
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 64; ++x) {
      const float dx = (float(x) - 31.5f) / 32, dy = (float(y) - 31.5f) / 32;
      const float fall = std::max(0.f, 1 - std::sqrt(dx * dx + dy * dy));
      auto *p = &pixels[(y * 64 + x) * 4];
      p[0] = p[1] = p[2] = 255;
      p[3] = uint8_t(std::min(255.f, fall * fall * (.6f + .4f * random.unit()) * 160));
    }
  const auto smokePath = texture(0xa1003, REMIXAPI_FORMAT_R8G8B8A8_SRGB, 64, pixels);
  // Leaves: overlapping opaque ellipses on a clear card, about half covered.
  std::fill(pixels.begin(), pixels.end(), 0);
  for (uint32_t k = 0; k < 9; ++k) {
    const float cx = random.range(12, 52), cy = random.range(12, 52), rx = random.range(5, 12),
                ry = random.range(3, 8);
    for (uint32_t y = 0; y < 64; ++y)
      for (uint32_t x = 0; x < 64; ++x) {
        const float dx = (float(x) - cx) / rx, dy = (float(y) - cy) / ry;
        if (dx * dx + dy * dy <= 1) {
          auto *p = &pixels[(y * 64 + x) * 4];
          p[0] = 60;
          p[1] = uint8_t(110 + random.next() % 60);
          p[2] = 40;
          p[3] = 255;
        }
      }
  }
  const auto leafPath = texture(0xa1004, REMIXAPI_FORMAT_R8G8B8A8_SRGB, 64, pixels);
  // The stains' paint: red where the atlas covers, clear elsewhere (UnityRTX's
  // blood paint texture).
  std::filesystem::path paintPath;
  {
    std::vector<uint8_t> paint(160 * 32 * 4, 0);
    for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 160; ++x) {
        const char digit = splatterAtlas[y][39 - x / 4];
        const uint32_t nibble = uint32_t(digit <= '9' ? digit - '0' : digit - 'a' + 10);
        auto *p = &paint[(y * 160 + x) * 4];
        p[0] = 255;
        p[3] = (nibble >> (x % 4)) & 1u ? 255 : 0;
      }
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = 0xa1005;
    info.width = 160;
    info.height = 32;
    info.depth = info.mipLevels = 1;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.data = paint.data();
    info.dataSize = paint.size();
    remixapi_TextureHandle handle;
    ok(api.CreateTexture(&info, &handle), "CreateTexture (paint)");
    paintPath = path(0xa1005);
  }

  auto material = [&](uint64_t hash, const std::filesystem::path &albedo, float roughness, bool blended,
                      uint8_t alphaReference, bool pointSampled) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = roughness;
    opaque.blendType_hasvalue = blended;
    opaque.blendType_value = blended ? 1 : 0;
    opaque.alphaTestType = alphaReference ? 6 : 7;
    opaque.alphaReferenceValue = alphaReference;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = hash;
    info.albedoTexture = albedo.c_str();
    info.filterMode = pointSampled ? 0 : 1;
    info.wrapModeU = info.wrapModeV = pointSampled ? 0 : 1;
    remixapi_MaterialHandle handle;
    ok(api.CreateMaterial(&info, &handle), "CreateMaterial");
    return handle;
  };
  const auto roomMaterial = material(0xa2000, roomPath, .6f, false, 0, false);
  // Blood: UnityRTX's names with "blood" in them get roughness .15.
  // REMIX_BENCH_BLOOD_CUTOUT=1: the drops alpha-tested instead (the game's own shader).
  const bool cutout = env("REMIX_BENCH_BLOOD_CUTOUT", 0) != 0;
  // REMIX_BENCH_BLOOD_ROUGHNESS (percent, [15]).
  const float bloodRoughness = float(env("REMIX_BENCH_BLOOD_ROUGHNESS", 15)) / 100;
  const auto dropMaterial = material(0xa2001, dropPath, bloodRoughness, !cutout, cutout ? 128 : 2, true);
  const auto trailMaterial = material(0xa2002, trailPath, bloodRoughness, !cutout, cutout ? 128 : 2, true);
  const auto smokeMaterial = material(0xa2003, smokePath, .7f, true, 2, false);
  const auto leafMaterial = material(0xa2004, leafPath, .6f, false, 127, false);
  remixapi_MaterialHandle paintMaterial;
  {
    remixapi_MaterialInfoPaintFinishEXT finish{};
    finish.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PAINT_FINISH_EXT;
    // REMIX_BENCH_PAINT_FINISH=0: the paint tints only (no wet finish).
    finish.enabled = env("REMIX_BENCH_PAINT_FINISH", 1) != 0;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &finish;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    // REMIX_BENCH_PAINT_ROUGHNESS (percent, [15]: UnityRTX's for blood).
    opaque.roughnessConstant = float(env("REMIX_BENCH_PAINT_ROUGHNESS", 15)) / 100;
    opaque.alphaTestType = 6;
    opaque.alphaReferenceValue = 127;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = 0xa2005;
    info.albedoTexture = paintPath.c_str();
    info.filterMode = 0;
    info.wrapModeU = info.wrapModeV = 1;
    ok(api.CreateMaterial(&info, &paintMaterial), "CreateMaterial (paint)");
  }

  // REMIX_BENCH_UPDATE=1: particle meshes say they may be updated in place
  // (remixapi_MeshInfoUpdateEXT); their counts are constant, so their BLASes refit.
  const bool update = env("REMIX_BENCH_UPDATE", 0) != 0;
  auto mesh = [&](uint64_t hash, Batch &batch, remixapi_MaterialHandle surfaceMaterial, bool replaced = false) {
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = batch.vertices.data();
    surface.vertices_count = batch.vertices.size();
    surface.indices_values = batch.indices.data();
    surface.indices_count = batch.indices.size();
    surface.material = surfaceMaterial;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshInfoUpdateEXT updateInfo{};
    updateInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT;
    updateInfo.enabled = 1;
    if (replaced && update)
      info.pNext = &updateInfo;
    remixapi_MeshHandle handle;
    ok(api.CreateMesh(&info, &handle), "CreateMesh");
    return handle;
  };
  auto retain = [&](remixapi_MeshHandle handle, uint64_t id) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = id;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &identity;
    instance.mesh = handle;
    instance.doubleSided = 1;
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
    uint64_t added = 0;
    ok(remixapi_AddSceneInstancesEXT(&instance, 1, &added), "AddSceneInstancesEXT");
  };

  // The room (Z up): a 24 x 24 floor, three 6 m walls and a few pillars, open above.
  Batch room;
  const V3 up{0, 0, 1};
  room.quad({0, 4, 0}, {12, 0, 0}, {0, 12, 0}, 0xffffffff);
  room.quad({0, 16, 3}, {-12, 0, 0}, {0, 0, 3}, 0xffffffff);
  room.quad({-12, 4, 3}, {0, -12, 0}, {0, 0, 3}, 0xffffffff);
  room.quad({12, 4, 3}, {0, 12, 0}, {0, 0, 3}, 0xffffffff);
  for (int k = 0; k < 4; ++k) {
    const V3 c{-6.f + 4.f * float(k), 9, 2};
    room.quad(c + V3{0, -.5f, 0}, {-.5f, 0, 0}, {0, 0, 2}, 0xffffffff);
    room.quad(c + V3{.5f, 0, 0}, {0, -.5f, 0}, {0, 0, 2}, 0xffffffff);
    room.quad(c + V3{-.5f, 0, 0}, {0, .5f, 0}, {0, 0, 2}, 0xffffffff);
  }
  retain(mesh(0xa3000, room, roomMaterial), 1);
  // Foliage: one retained mesh of randomly turned leaf cards in a bush ahead.
  if (leafCount) {
    Batch leaves;
    for (uint32_t i = 0; i < leafCount; ++i) {
      const V3 c{random.range(-4, 4), random.range(2, 7), random.range(.2f, 3.5f)};
      const V3 a = normalize({random.range(-1, 1), random.range(-1, 1), random.range(-1, 1)});
      const V3 b = normalize(cross(a, normalize({random.range(-1, 1), random.range(-1, 1), random.range(-1, 1)})));
      leaves.quad(c, a * .3f, b * .3f, 0xffffffff);
    }
    retain(mesh(0xa3001, leaves, leafMaterial), 2);
  }

  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {.3f, .5f, -.8f};
  distant.angularDiameterDegrees = .5f;
  remixapi_LightInfo sunInfo{};
  sunInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  sunInfo.pNext = &distant;
  sunInfo.hash = 0xa4000;
  sunInfo.radiance = {3, 3, 3};
  remixapi_LightHandle sun;
  ok(api.CreateLight(&sunInfo, &sun), "CreateLight (sun)");
  std::vector<remixapi_LightHandle> lights(6);
  for (uint32_t i = 0; i < lights.size(); ++i) {
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {-8.f + 3.2f * float(i), 6.f + 4.f * float(i % 2), 3.5f};
    sphere.radius = .15f;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &sphere;
    light.hash = 0xa4001 + i;
    light.radiance = {30, 20, 12};
    ok(api.CreateLight(&light, &lights[i]), "CreateLight");
  }

  // Camera: standing in the room, looking along +Y a little down (swaying in submit).
  const V3 eye{0, -5, 1.7f}, forward = normalize({0, 1, -.08f});
  const V3 right = normalize(cross(forward, up)), cameraUp = cross(right, forward);
  Batch dropBatch, trailBatch, faintBatch, smokeBatch;
  // A particle batch's mesh: created again under the same hash each frame,
  // which replaces it (its handle is the hash, as UnityRTX keeps it).
  struct Dynamic {
    uint64_t hash;
  };
  Dynamic dropMesh{0xa5001}, trailMesh{0xa5002}, faintMesh{0xa5003}, smokeMesh{0xa5004};
  constexpr float step = 1.f / 60;
  const bool freeze = env("REMIX_BENCH_FREEZE", 0) != 0;
  // A drop's position: spray k, particle i, at time t (fixed seeds per life).
  auto particle = [&](uint32_t k, uint32_t i, float t, float &age, float &life, V3 &position, V3 &velocity) {
    Random seed{uint64_t(k) * 7919 + i};
    life = seed.range(.8f, 1.4f);
    const float offset = seed.range(0, life), cycle = (t + offset) / life;
    age = (cycle - std::floor(cycle)) * life;
    Random shot{(uint64_t(k) << 40 | uint64_t(i) << 20 | uint64_t(cycle)) * 2654435761ull + 7};
    const V3 origin{-2.5f + 5.f * float(k) / float(std::max(1u, sprays - 1)), -1.f + 2.f * float(k % 2), 1.4f};
    const V3 v{shot.range(-3, 3), shot.range(-3, 3), shot.range(2, 8)};
    const float g = -9.8f * 1.5f;
    position = origin + v * age + V3{0, 0, .5f * g * age * age};
    velocity = v + V3{0, 0, g * age};
    if (position.z < .02f) {
      position.z = .02f;
      velocity = {0, 0, 0};
    }
  };
  auto draw = [&](Dynamic &dynamic, Batch &batch, remixapi_MaterialHandle surfaceMaterial, uint64_t id,
                  bool blended = true) {
    if (batch.indices.empty())
      return;
    const auto slot = mesh(dynamic.hash, batch, surfaceMaterial, true);
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaBlendEnabled = 1;
    blend.srcColorBlendFactor = 6; // SRC_ALPHA
    blend.dstColorBlendFactor = 7; // ONE_MINUS_SRC_ALPHA
    blend.textureColorArg1Source = 1;
    blend.textureColorArg2Source = 2;
    blend.textureColorOperation = 3; // texel * vertex
    blend.textureAlphaArg1Source = 1;
    blend.textureAlphaArg2Source = 2;
    blend.textureAlphaOperation = 3;
    blend.tFactor = 0xffffffffu;
    blend.srcAlphaBlendFactor = 1;
    blend.dstAlphaBlendFactor = 7;
    blend.writeMask = 15;
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = blended ? &blend : nullptr;
    identity.instanceId = id;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &identity;
    instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE;
    instance.mesh = slot;
    instance.doubleSided = 1;
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
    ok(api.DrawInstance(&instance), "DrawInstance");
  };

  // Stain i: three in four on the floor ahead, the rest on the back wall and
  // the pillars' fronts; turned at random, 0.4-1.4 m across, lifted 1 cm.
  Batch stainBatch;
  uint32_t stainFirst = ~0u;
  auto stainQuads = [&](uint32_t first) {
    stainBatch.clear();
    for (uint32_t i = first; i < first + stainCount; ++i) {
      Random seed{uint64_t(i) * 15485863ull + 11};
      const float size = seed.range(.2f, .7f), angle = seed.range(0, 6.2831853f);
      V3 centre, a, b;
      if (seed.unit() < .75f) {
        centre = {seed.range(-5, 5), seed.range(-1, 9), .01f};
        a = {std::cos(angle), std::sin(angle), 0};
        b = {-std::sin(angle), std::cos(angle), 0};
      } else if (seed.unit() < .5f) {
        centre = {seed.range(-8, 8), 15.99f, seed.range(.3f, 4)};
        a = {std::cos(angle), 0, std::sin(angle)};
        b = {-std::sin(angle), 0, std::cos(angle)};
      } else {
        centre = {-6.f + 4.f * float(seed.next() % 4) + seed.range(-.3f, .3f), 8.49f, seed.range(.3f, 3.7f)};
        a = {std::cos(angle), 0, std::sin(angle)};
        b = {-std::sin(angle), 0, std::cos(angle)};
      }
      const auto base = uint32_t(stainBatch.vertices.size());
      const V3 n = normalize(cross(a, b));
      const float u = float(i % 5) * .2f;
      stainBatch.vertex(centre - a * size - b * size, n, u, 0, 0xffffffff);
      stainBatch.vertex(centre + a * size - b * size, n, u + .2f, 0, 0xffffffff);
      stainBatch.vertex(centre + a * size + b * size, n, u + .2f, 1, 0xffffffff);
      stainBatch.vertex(centre - a * size + b * size, n, u, 1, 0xffffffff);
      stainBatch.indices.insert(stainBatch.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
  };
  remixapi_MeshHandle stainMesh = nullptr;
  auto drawStains = [&](uint32_t frame) {
    if (!stainCount)
      return;
    const uint32_t first = stainEvery ? frame / stainEvery : 0;
    if (first != stainFirst) {
      stainFirst = first;
      stainQuads(first);
      stainMesh = mesh(0xa5005, stainBatch, paintMaterial);
    }
    // Multiplicative paint: the receiver's colour times the paint's.
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaBlendEnabled = 1;
    blend.srcColorBlendFactor = 4; // DST_COLOR
    blend.dstColorBlendFactor = 0; // ZERO
    blend.textureColorArg1Source = 1;
    blend.textureColorArg2Source = 2;
    blend.textureColorOperation = 3;
    blend.textureAlphaArg1Source = 1;
    blend.textureAlphaArg2Source = 2;
    blend.textureAlphaOperation = 3;
    blend.tFactor = 0xffffffffu;
    blend.srcAlphaBlendFactor = 0;
    blend.dstAlphaBlendFactor = 1;
    blend.writeMask = 15;
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = &blend;
    identity.instanceId = 0x40000005;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &identity;
    instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC;
    instance.mesh = stainMesh;
    instance.doubleSided = 1;
    instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
    ok(api.DrawInstance(&instance), "DrawInstance (stains)");
  };

  auto submit = [&](uint32_t frame) {
    ok(api.PumpEvents(), "PumpEvents");
    remixapi_CameraInfoParameterizedEXT parameters{};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    // A slight sway, as a player never stands still: every frame is traced.
    const float sway = .03f * std::sin(float(frame) * step * 2);
    parameters.position = {eye.x + sway, eye.y, eye.z};
    parameters.forward = {forward.x, forward.y, forward.z};
    parameters.up = {cameraUp.x, cameraUp.y, cameraUp.z};
    parameters.right = {right.x, right.y, right.z};
    parameters.fovYInDegrees = 70;
    parameters.aspect = float(width) / float(height);
    parameters.nearPlane = .05f;
    parameters.farPlane = 500;
    remixapi_CameraInfo camera{};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
    ok(api.SetupCamera(&camera), "SetupCamera");
    ok(api.DrawLightInstance(sun), "DrawLightInstance (sun)");
    for (auto light : lights)
      ok(api.DrawLightInstance(light), "DrawLightInstance");
    ok(remixapi_DrawSceneInstancesEXT(nullptr), "DrawSceneInstancesEXT");

    // REMIX_BENCH_FREEZE=1: the particles hold still (their meshes unchanged).
    const float t = (freeze ? 100.f : float(frame)) * step;
    dropBatch.clear();
    trailBatch.clear();
    faintBatch.clear();
    smokeBatch.clear();
    const uint32_t red = color(.85f, 0, 0, 1), faint = color(.86f, 0, 0, .78f);
    for (uint32_t k = 0; k < sprays; ++k) {
      for (uint32_t i = 0; i < drops; ++i) {
        float age, life;
        V3 p, v;
        particle(k, i, t, age, life, p, v);
        dropBatch.quad(p, right * .25f, cameraUp * .25f, red);
        // The trail: the drop's last positions, a ribbon turned to the camera,
        // its texture stretched along it.
        constexpr uint32_t points = 4;
        V3 trail[points];
        trail[0] = p;
        for (uint32_t j = 1; j < points; ++j) {
          float a2, l2;
          V3 w;
          particle(k, i, std::max(t - float(j) * 2 * step, t - age), a2, l2, trail[j], w);
        }
        const auto base = uint32_t(trailBatch.vertices.size());
        for (uint32_t j = 0; j < points; ++j) {
          const V3 q = trail[j], along = normalize(j + 1 < points ? q - trail[j + 1] : trail[j - 1] - q);
          const V3 side = normalize(cross(along, eye - q)) * .06f;
          const float u = float(j) / float(points - 1);
          trailBatch.vertex(q - side, normalize(eye - q), u, 1, red);
          trailBatch.vertex(q + side, normalize(eye - q), u, 0, red);
        }
        for (uint32_t j = 0; j + 1 < points; ++j) {
          const uint32_t a = base + 2 * j;
          trailBatch.indices.insert(trailBatch.indices.end(), {a, a + 2, a + 3, a, a + 3, a + 1});
        }
      }
      for (uint32_t i = 0; i < faintDrops; ++i) {
        float age, life;
        V3 p, v;
        particle(k, drops + i, t, age, life, p, v);
        faintBatch.quad(p, right * .15f, cameraUp * .15f, faint);
      }
    }
    for (uint32_t i = 0; i < smokeCount; ++i) {
      Random seed{uint64_t(i) * 104729 + 3};
      const float size = seed.range(1.2f, 2), rise = seed.range(.1f, .4f), phase = seed.range(0, 6.28f);
      const V3 c{seed.range(-4, 4), seed.range(1, 7), std::fmod(seed.range(0, 4) + rise * t, 4.f) + .5f};
      const float shade = seed.range(.35f, .6f);
      smokeBatch.quad(c + V3{.3f * std::sin(t + phase), 0, 0}, right * size, cameraUp * size,
                      color(shade, shade, shade, 1));
    }
    draw(dropMesh, dropBatch, dropMaterial, 0x40000001, !cutout);
    draw(trailMesh, trailBatch, trailMaterial, 0x40000002, !cutout);
    draw(faintMesh, faintBatch, dropMaterial, 0x40000003);
    draw(smokeMesh, smokeBatch, smokeMaterial, 0x40000004);
    drawStains(frame);
    ok(api.Present(nullptr), "Present");
  };

  std::printf("alpha frame: %s; %u sprays of %u drops (+%u faint), %u stains, %u smoke billboards, %u leaves, %ux%u\n",
              cases.c_str(), sprays, drops, faintDrops, stainCount, smokeCount, leafCount, width, height);
  std::fflush(stdout);
  uint32_t frame = 0;
  auto prepared = remix::test::preparedFrame([&] { submit(frame); });
  prepared();
  for (uint32_t i = 0; i < warmup; ++i)
    prepared(), ++frame;

  // Pass self times by name, in the order passes first appear.
  std::vector<std::string> order;
  std::map<std::string, Stats> passes;
  Stats gpuFrame, untimed;
  uint64_t lastMeasured = 0;
  uint32_t renderWidth = 0, renderHeight = 0, measured = 0;
  const auto wall = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < frames; ++i) {
    submit(frame++);
    std::lock_guard lock(remix::api::g_apiMutex);
    const auto &timings = remix::api::g_runtime.gpuPassTimings();
    if (!timings.available || timings.frame == lastMeasured)
      continue;
    lastMeasured = timings.frame;
    ++measured;
    renderWidth = timings.renderWidth;
    renderHeight = timings.renderHeight;
    gpuFrame.add(timings.frameMilliseconds);
    untimed.add(timings.untimedMilliseconds);
    std::map<std::string, double> self;
    for (const auto &pass : timings.passes)
      self[pass.name] += pass.selfMilliseconds;
    for (const auto &[name, ms] : self) {
      if (!passes.count(name))
        order.push_back(name);
      passes[name].add(ms);
    }
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall).count();
  std::printf("measured %u GPU frames of %u, %.1f FPS wall clock, render %ux%u\n", measured, frames,
              double(frames) / seconds, renderWidth, renderHeight);
  std::printf("%-58s %8.3f %8.3f\n", "GPU frame (mean, median ms)", gpuFrame.mean(), gpuFrame.median());
  std::printf("%-58s %8.3f %8.3f\n", "between passes", untimed.mean(), untimed.median());
  // Passes missing from a frame count as zero there.
  std::stable_sort(order.begin(), order.end(), [&](const auto &a, const auto &b) {
    return passes[a].mean() * double(passes[a].samples.size()) > passes[b].mean() * double(passes[b].samples.size());
  });
  for (const auto &name : order) {
    const auto &s = passes[name];
    const double mean = s.mean() * double(s.samples.size()) / double(std::max(1u, measured));
    if (mean >= .005)
      std::printf("  %-56s %8.3f %8.3f\n", name.c_str(), mean, s.median());
  }
  if (const char *shot = std::getenv("REMIX_BENCH_SCREENSHOT")) {
    const auto target = std::filesystem::absolute(shot).string();
    ok(remix::test::requestScreenshot(api, target.c_str(), [&] { submit(frame++); }), "screenshot");
    std::printf("screenshot %s\n", target.c_str());
  }
  ok(api.Shutdown(), "Shutdown");
  return 0;
}
