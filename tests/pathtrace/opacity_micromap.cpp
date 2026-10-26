// rtx.native.pathtrace.opacityMicromaps renders the same image and resolves
// alpha-tested traversal in hardware: ray-traced primary visibility and hard
// sun shadows through a foliage-like cutout, captured with micromaps off, on,
// and off again. The two off captures establish that the fixture itself is
// deterministic; the on capture must match them byte for byte, and the ray
// profile must show far fewer shader alpha tests.
#include "api/exports.h"
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <remix/remix_native_ext.h>
#include <sstream>
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
  void ok(remixapi_ErrorCode result) { require(!result, "Opacity micromap API failure"); }
  constexpr uint32_t width = 192, height = 192, textureSize = 64, grid = 8;

  // Last row of the ray profile: shader alpha tests and candidates of every
  // traced ray (primary visibility and sun shadows here).
  struct Rays {
    double alpha = 0, candidates = 0, queries = 0;
  };
  Rays lastProfile(const std::filesystem::path &file) {
    std::ifstream input(file);
    std::string header, line, last;
    require(std::getline(input, header).good(), "Missing ray profile");
    while (std::getline(input, line))
      if (!line.empty()) last = line;
    std::vector<std::string> names, values;
    for (auto *out : {&names, &values}) {
      std::stringstream stream(out == &names ? header : last);
      std::string cell;
      while (std::getline(stream, cell, ',')) out->push_back(cell);
    }
    require(names.size() == values.size(), "Malformed ray profile");
    Rays rays;
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i].ends_with("_alpha")) rays.alpha += std::stod(values[i]);
      if (names[i].ends_with("_candidates")) rays.candidates += std::stod(values[i]);
      if (names[i].ends_with("_queries")) rays.queries += std::stod(values[i]);
    }
    return rays;
  }
} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "Supply screenshot directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  const auto profile = directory / "rays.csv";
  std::filesystem::remove(profile);
  // The renderer reads the C runtime environment (SDL's setenv does not
  // update it on Windows).
#if defined(_WIN32)
  _putenv_s("REMIX_RAY_PROFILE", profile.string().c_str());
  _putenv_s("REMIX_PROFILE_EVERY_FRAME", "1");
#else
  setenv("REMIX_RAY_PROFILE", profile.string().c_str(), 1);
  setenv("REMIX_PROFILE_EVERY_FRAME", "1", 1);
#endif
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = width;
  startup.windowHeight = height;
#endif
  ok(api.Startup(&startup));
  SDL_SetWindowSize(remix::api::g_runtime.window().handle(), int(width), int(height));
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
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  // Deterministic direct lighting: ray-traced primary visibility and one
  // hard-shadowed sun, without temporal or stochastic passes.
  for (const char *key : {"sharc", "sigma", "taa", "bounces", "autoExposure", "bloom",
                          "accumulate", "localLights", "sky"})
    config(key, "0");
  config("primaryVisibility", "ray");
  config("sunAngularDiameter", "0");
  config("denoiser", "off");
  config("upscaler", "off");
  config("samples", "1");

  remixapi_CameraInfoParameterizedEXT parameters{};
  parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  parameters.right = {1, 0, 0};
  parameters.up = {0, 1, 0};
  parameters.forward = {0, 0, 1};
  parameters.aspect = 1;
  parameters.fovYInDegrees = 70;
  parameters.nearPlane = .1f;
  parameters.farPlane = 100;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &parameters;

  // Foliage-like coverage: overlapping leaves with soft alpha edges and noise,
  // so triangles mix opaque, cut-out and edge micro-triangles.
  std::vector<uint8_t> pixels(size_t(textureSize) * textureSize * 4);
  uint32_t seed = 7;
  auto random = [&] {
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) / float(1u << 24);
  };
  float leaves[12][3];
  for (auto &leaf : leaves) leaf[0] = random(), leaf[1] = random(), leaf[2] = .06f + .1f * random();
  for (uint32_t y = 0; y < textureSize; ++y)
    for (uint32_t x = 0; x < textureSize; ++x) {
      float coverage = 0;
      for (const auto &leaf : leaves) {
        const float d = std::hypot((x + .5f) / textureSize - leaf[0], (y + .5f) / textureSize - leaf[1]);
        coverage = std::max(coverage, std::clamp((leaf[2] - d) * 30 + .5f, 0.f, 1.f));
      }
      auto *p = &pixels[4 * (size_t(y) * textureSize + x)];
      p[0] = uint8_t(40 + 150 * coverage);
      p[1] = uint8_t(90 + 160 * coverage);
      p[2] = 40;
      p[3] = uint8_t(std::clamp(coverage + (random() - .5f) * .1f, 0.f, 1.f) * 255);
    }
  remixapi_TextureInfo texture{};
  texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  texture.hash = 1;
  texture.width = textureSize;
  texture.height = textureSize;
  texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  texture.data = pixels.data();
  texture.dataSize = pixels.size();
  remixapi_TextureHandle textureHandle{};
  ok(api.CreateTexture(&texture, &textureHandle));
#ifdef _WIN32
  const auto texturePath = L"0x1";
#else
  const auto texturePath = "0x1";
#endif
  auto material = [&](uint64_t hash, bool cutout) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = 1;
    opaque.alphaTestType = cutout ? 4 : 7; // GREATER, ALWAYS
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = hash;
    info.albedoTexture = cutout ? texturePath : nullptr;
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  // A grid of cells, so opacity partitions mix solid and alpha-tested triangles.
  // Skinned planes follow one bone, refitting their BLAS as it moves.
  auto plane = [&](uint64_t hash, remixapi_MaterialHandle mat, float z, float halfSize, uint32_t cells,
                   bool skinned = false) {
    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<uint32_t> indices;
    for (uint32_t y = 0; y <= cells; ++y)
      for (uint32_t x = 0; x <= cells; ++x) {
        remixapi_HardcodedVertex v{};
        v.position[0] = (float(x) / cells * 2 - 1) * halfSize;
        v.position[1] = (1 - float(y) / cells * 2) * halfSize;
        v.position[2] = z;
        v.normal[2] = -1;
        v.texcoord[0] = float(x) / cells;
        v.texcoord[1] = float(y) / cells;
        v.color = 0xffffffff;
        vertices.push_back(v);
      }
    for (uint32_t y = 0; y < cells; ++y)
      for (uint32_t x = 0; x < cells; ++x) {
        const uint32_t i = y * (cells + 1) + x;
        for (uint32_t k : {i, i + 1, i + cells + 2, i, i + cells + 2, i + cells + 1}) indices.push_back(k);
      }
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices.data();
    surface.vertices_count = vertices.size();
    surface.indices_values = indices.data();
    surface.indices_count = indices.size();
    surface.material = mat;
    std::vector<float> weights(vertices.size(), 1.f);
    std::vector<uint32_t> bones(vertices.size(), 0);
    if (skinned) {
      surface.skinning_hasvalue = 1;
      surface.skinning_value = {1, weights.data(), uint32_t(weights.size()), bones.data(), uint32_t(bones.size())};
    }
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle result{};
    ok(api.CreateMesh(&info, &result));
    return result;
  };
  const auto wall = plane(10, material(2, false), 5, 6, 1);
  const auto cutout = material(3, true);
  const auto foliage = plane(11, cutout, 2.5f, 1.6f, grid);
  const auto skinnedFoliage = plane(12, cutout, 2.5f, 1.6f, grid, true);
  bool skin = false;
  remixapi_Transform bone{};
  for (int i = 0; i < 3; ++i) bone.matrix[i][i] = 1;
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  const float length = std::sqrt(.3f * .3f + .4f * .4f + 1.f);
  distant.direction = {.3f / length, -.4f / length, 1 / length};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 20;
  light.radiance = {3, 3, 3};
  remixapi_LightHandle sun{};
  ok(api.CreateLight(&light, &sun));

  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(sun));
    uint64_t id = 1;
    for (auto mesh : {wall, skin ? skinnedFoliage : foliage}) {
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = id++;
      identity.rasterVisible = 1;
      remixapi_InstanceInfoBoneTransformsEXT bones{};
      bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
      bones.pNext = &identity;
      bones.boneTransforms_count = 1;
      bones.boneTransforms_values = &bone;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = mesh == skinnedFoliage ? static_cast<void *>(&bones) : &identity;
      instance.mesh = mesh;
      instance.doubleSided = 1;
      for (int i = 0; i < 3; ++i) instance.transform.matrix[i][i] = 1;
      ok(api.DrawInstance(&instance));
    }
    ok(api.Present(nullptr));
  });
  struct Capture {
    std::vector<uint8_t> pixels;
    Rays rays;
  };
  auto capture = [&](const std::string &name) {
    for (unsigned i = 0; i < 12; ++i) frame(); // Settle BLAS rebuilds and pipelines.
    const auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, channels;
    auto *data = stbi_load(file.c_str(), &w, &h, &channels, 4);
    require(data && w == int(width) && h == int(height), "Opacity micromap capture unavailable");
    Capture result{{data, data + size_t(w) * h * 4}, lastProfile(profile)};
    stbi_image_free(data);
    std::printf("%s: queries %.0f, candidates %.0f, alpha tests %.0f\n", name.c_str(), result.rays.queries,
                result.rays.candidates, result.rays.alpha);
    return result;
  };
  auto differing = [](const Capture &a, const Capture &b) {
    size_t count = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) count += a.pixels[i] != b.pixels[i];
    return count;
  };
  const auto off = capture("off");
  // The fixture must exercise cut-out coverage in primary and shadow rays
  // (the profile samples a subset of pixels).
  require(off.rays.queries > 100 && off.rays.alpha > off.rays.queries * .3, "Fixture reached too few alpha tests");
  size_t lit = 0, dark = 0;
  for (size_t i = 0; i < off.pixels.size(); i += 4) {
    lit += off.pixels[i + 1] > 60;
    dark += off.pixels[i + 1] < 20;
  }
  require(lit > width * height / 10 && dark > width * height / 50, "Fixture lacks lit and shadowed regions");
  config("opacityMicromaps", "1");
  const auto on = capture("on");
  config("opacityMicromaps", "0");
  const auto again = capture("off-again");
  std::printf("differing bytes: off/off-again %zu, off/on %zu\n", differing(off, again), differing(off, on));
  require(differing(off, again) == 0, "Fixture is not deterministic; the comparison would be meaningless");
  require(differing(off, on) == 0, "Opacity micromaps changed the image");
  require(on.rays.alpha < off.rays.alpha * .5, "Opacity micromaps did not remove most shader alpha tests");
  require(again.rays.alpha > off.rays.alpha * .9, "Disabling opacity micromaps did not restore the shader tests");

  // A skinned copy refits its BLAS with the micromap attached; each capture
  // first moves the bone through several poses, then holds the last one.
  auto statistics = [&] {
    remixapi_TracingStatisticsEXT s{};
    s.structSize = sizeof(s);
    ok(remixapi_GetTracingStatisticsEXT(&s));
    return s;
  };
  skin = true;
  auto posed = [&](const std::string &name) {
    const auto before = statistics();
    for (int pose = 0; pose < 6; ++pose) {
      bone.matrix[0][3] = .02f * float(pose);
      bone.matrix[1][3] = -.01f * float(pose);
      frame();
    }
    const auto after = statistics();
    require(after.blasRefits >= before.blasRefits + 4, "Skinned foliage did not refit its BLAS");
    return capture(name);
  };
  const auto skinOff = posed("skinned-off");
  config("opacityMicromaps", "1");
  const auto skinOn = posed("skinned-on");
  std::printf("skinned differing bytes: %zu\n", differing(skinOff, skinOn));
  require(differing(skinOff, skinOn) == 0, "Opacity micromaps changed the refitted skinned image");
  require(skinOn.rays.alpha < skinOff.rays.alpha * .5, "Refitted skinned BLAS lost its micromap");
  ok(api.Shutdown());
  std::puts("PASS");
  return 0;
}
