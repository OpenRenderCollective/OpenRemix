// Path-traced view models (rtx.native.pathtrace.viewModel): a VIEW_MODEL
// camera instance is traced in the world camera's view, in front of the scene
// even where it reaches into a wall, and casts no shadow on the world. Each
// check has a control: the same quad as a world instance is hidden by the wall
// and does cast the shadow. Raster and ray-traced primary visibility alike.
#include "runtime/prepared.h"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
void check(remixapi_ErrorCode code, int line) {
  if (code) {
    std::fprintf(stderr, "FAIL: API call failed (code %d, line %d)\n", int(code), line);
    std::exit(1);
  }
}
#define ok(call) check((call), __LINE__)

constexpr uint32_t kWidth = 640, kHeight = 480;

remixapi_MaterialHandle material(remixapi_Interface &api, uint64_t hash, float r, float g, float b) {
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {r, g, b};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = hash;
#ifdef _WIN32
  info.albedoTexture = L"";
#else
  info.albedoTexture = "";
#endif
  remixapi_MaterialHandle handle = nullptr;
  ok(api.CreateMaterial(&info, &handle));
  return handle;
}

// A square of half size 1 in the z = 0 plane, facing -z (the camera).
remixapi_MeshHandle quad(remixapi_Interface &api, uint64_t hash, remixapi_MaterialHandle surfaceMaterial) {
  remixapi_HardcodedVertex vertices[4] = {};
  const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  for (int i = 0; i < 4; ++i) {
    vertices[i].position[0] = corners[i][0];
    vertices[i].position[1] = corners[i][1];
    vertices[i].normal[2] = -1;
    vertices[i].color = 0xffffffff;
  }
  const uint32_t indices[] = {0, 2, 1, 0, 3, 2};
  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices;
  surface.vertices_count = 4;
  surface.indices_values = indices;
  surface.indices_count = 6;
  surface.material = surfaceMaterial;
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = hash;
  info.surfaces_count = 1;
  info.surfaces_values = &surface;
  remixapi_MeshHandle handle = nullptr;
  ok(api.CreateMeshBatched(&info, &handle));
  return handle;
}

float eyeX = 0; // the cameras' position along x (scene 4 moves them)
void camera(remixapi_Interface &api, remixapi_CameraType type) {
  remixapi_CameraInfoParameterizedEXT params = {};
  params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  params.position = {eyeX, 0, 0};
  params.forward = {0, 0, 1};
  params.up = {0, 1, 0};
  params.right = {1, 0, 0};
  params.aspect = float(kWidth) / kHeight;
  params.fovYInDegrees = 60;
  params.nearPlane = .05f;
  params.farPlane = 100;
  remixapi_CameraInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  info.pNext = &params;
  info.type = type;
  ok(api.SetupCamera(&info));
}

// mask: 0 none; CUT or SOURCE of group 1 (remixapi_InstanceInfoMaskEXT).
void draw(remixapi_Interface &api, remixapi_MeshHandle mesh, float scale, float z, bool viewModel,
          bool rasterOnly = false, int mask = -1) {
  remixapi_InstanceInfoMaskEXT maskInfo = {};
  maskInfo.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT;
  maskInfo.group = 1;
  maskInfo.role = mask < 0 ? 0u : uint32_t(mask);
  remixapi_InstanceIdentityEXT identity = {};
  identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
  identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
  identity.rasterVisible = 1;
  remixapi_InstanceInfo instance = {};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.pNext = rasterOnly ? static_cast<void *>(&identity) : mask >= 0 ? static_cast<void *>(&maskInfo) : nullptr;
  instance.mesh = mesh;
  instance.doubleSided = 1;
  instance.categoryFlags = viewModel ? REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL : 0;
  instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = scale;
  instance.transform.matrix[2][2] = 1;
  instance.transform.matrix[2][3] = z;
  instance.transform.matrix[0][3] = eyeX; // in front of the eye
  ok(api.DrawInstance(&instance));
}

struct Image {
  int width = 0, height = 0;
  std::vector<unsigned char> pixels;
  // Mean RGB over a 9x9 window.
  std::array<float, 3> at(int x, int y) const {
    std::array<float, 3> sum = {};
    int count = 0;
    for (int dy = -4; dy <= 4; ++dy)
      for (int dx = -4; dx <= 4; ++dx) {
        const int px = x * width / int(kWidth) + dx, py = y * height / int(kHeight) + dy;
        if (px < 0 || py < 0 || px >= width || py >= height)
          continue;
        for (int c = 0; c < 3; ++c)
          sum[c] += pixels[(size_t(py) * width + px) * 4 + c];
        ++count;
      }
    for (auto &v : sum)
      v /= std::max(count, 1);
    return sum;
  }
};
float luminance(const std::array<float, 3> &c) { return .2126f * c[0] + .7152f * c[1] + .0722f * c[2]; }
// The lit red quad (tonemapped: salmon), not the grey wall.
bool red(const std::array<float, 3> &c) { return c[0] > 40 && c[0] > 1.4f * c[1] && c[0] > 1.4f * c[2]; }
} // namespace

int main(int argc, char **argv) {
  require(argc > 1, "Expected a capture directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.windowWidth = kWidth;
  startup.windowHeight = kHeight;
  ok(api.Startup(&startup));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2)) {
    ok(api.Shutdown());
    return 77; // no path tracing on this device
  }
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  ok(api.SetConfigVariable("rtx.native.vsync", "false"));
  for (const char *off : {"upscaler", "denoiser"})
    config(off, "off");
  for (const char *off : {"taa", "sharc", "sigma"})
    config(off, "false");
  config("bounces", "0");
  config("viewModel", "1");
  config("viewModelScale", "0.5");
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));

  // The sun travels toward +x and +z: a quad at z = 2 shadows the wall (z = 5)
  // 2.25 units to its right. Hard (no angular size): no noise.
  remixapi_LightInfoDistantEXT distant = {};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {.6f, 0, .8f};
  distant.angularDiameterDegrees = 0;
  remixapi_LightInfo light = {};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 0x201;
  light.radiance = {4, 4, 4};
  remixapi_LightHandle sun = nullptr;
  ok(api.CreateLight(&light, &sun));
  const auto white = material(api, 0x202, .8f, .8f, .8f), redMaterial = material(api, 0x203, .9f, .05f, .05f);
  const auto wall = quad(api, 0x204, white), occluder = quad(api, 0x205, redMaterial);

  // scene: 0 the shadow layout, 1 the behind-the-wall layout. viewModel: the
  // red quad as a view model (in its camera's space, before the 0.5 shrink)
  // or as world geometry where the view model ends up.
  int scene = 0;
  bool viewModel = true;
  auto frame = [&] {
    ok(api.PumpEvents());
    ok(api.DrawLightInstance(sun));
    camera(api, REMIXAPI_CAMERA_TYPE_WORLD);
    camera(api, REMIXAPI_CAMERA_TYPE_VIEW_MODEL);
    if (scene != 4)
      draw(api, wall, 20, 5, false);
    // Shadow: world z = 2, half size .25. Behind the wall: world z = 8, half
    // size 1. Near: a raster-only quad at world z = .03, nearer than the world
    // camera's near plane (.05), half size .01.
    const float worldScale = scene == 2 ? .01f : scene ? 1.f : .25f, worldZ = scene == 2 ? .03f : scene ? 8.f : 2.f;
    if (scene == 4) {
      // Precision: the cameras far from the origin; the view model fixed
      // in front of them must stay on the same pixels wherever they are.
      draw(api, occluder, .5f, 4, true);
    } else if (scene == 3) {
      // A hole: the quad of the shadow layout, cut (CUT) where a small mask
      // source (world half size .05) covers its centre; view models or world.
      const float k = viewModel ? 2 : 1;
      draw(api, occluder, .25f * k, 2 * k, viewModel, false, REMIXAPI_INSTANCE_MASK_CUT_EXT);
      // The source in front of the cut surface, as a scope's hide model is.
      draw(api, occluder, .05f * k * 1.95f / 2, 1.95f * k, viewModel, false, REMIXAPI_INSTANCE_MASK_SOURCE_EXT);
    } else if (viewModel)
      draw(api, occluder, worldScale * 2, worldZ * 2, true, scene == 2);
    else
      draw(api, occluder, worldScale, worldZ, false, scene == 2);
    ok(api.Present(nullptr));
  };
  auto settled = remix::test::preparedFrame(frame);
  auto capture = [&](const std::string &name) {
    settled();
    // Variants such as mask pipelines compile on first use in the background
    // (until then nothing is cut): wait until every requested one is done.
    for (int i = 0; i < 6000; ++i) {
      frame();
      const auto p = remix::api::g_runtime.preparationStatus().pipelines;
      if (p.completed + p.failed >= p.requested && i >= 3)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int i = 0; i < 4; ++i)
      frame();
    const auto path = (directory / (name + ".png")).string();
    std::filesystem::remove(path);
    ok(api.RequestPresentedScreenshot(path.c_str()));
    // Asynchronous presentation writes it a few frames later.
    for (int i = 0; i < 120 && !std::filesystem::exists(path); ++i)
      frame();
    frame();
    Image image;
    int channels = 0;
    auto *pixels = stbi_load(path.c_str(), &image.width, &image.height, &channels, 4);
    require(pixels && image.width > 0 && image.height > 0, "Missing captured image");
    image.pixels.assign(pixels, pixels + size_t(image.width) * image.height * 4);
    stbi_image_free(pixels);
    return image;
  };
  // The wall where the quad's shadow falls (x = 2.25 at z = 5), and lit wall
  // well away from it.
  const int shadowX = 507, litX = 507, litY = 120, centreX = int(kWidth) / 2, centreY = int(kHeight) / 2;
  // Raster and ray-traced primary visibility; then asynchronous presentation
  // (the render thread traces a published copy of the frame).
  for (const char *visibility : {"raster", "ray", "async"}) {
    const bool async = std::string(visibility) == "async";
    config("primaryVisibility", async ? "raster" : visibility);
    ok(api.SetConfigVariable("rtx.native.asyncPresent", async ? "1" : "0"));
    const std::string prefix = visibility;
    scene = 0;
    viewModel = false;
    const auto worldShadow = capture(prefix + "-shadow-world");
    viewModel = true;
    const auto modelShadow = capture(prefix + "-shadow-viewmodel");
    scene = 1;
    viewModel = false;
    const auto worldBehind = capture(prefix + "-behind-world");
    viewModel = true;
    const auto modelBehind = capture(prefix + "-behind-viewmodel");
    scene = 2;
    viewModel = false;
    const auto worldNear = capture(prefix + "-near-world");
    viewModel = true;
    const auto modelNear = capture(prefix + "-near-viewmodel");
    // After view-model frames, with the mask pipelines first requested now:
    // a compiled pipeline is used without waiting for the cache to be saved.
    scene = 3;
    viewModel = false;
    const auto worldCut = capture(prefix + "-cut-world");
    viewModel = true;
    const auto modelCut = capture(prefix + "-cut-viewmodel");
    scene = 4;
    std::vector<float> centroids;
    for (float x : {0.f, 7321.37f, 15873.61f, 30517.9f}) {
      eyeX = x;
      const auto image = capture(prefix + "-far-" + std::to_string(int(x)));
      // The red quad's centroid along the centre row, to a fraction of a pixel.
      double weight = 0, sum = 0;
      for (int px = 0; px < image.width; ++px) {
        const auto *c = &image.pixels[(size_t(image.height / 2) * image.width + px) * 4];
        const double w = std::clamp((double(c[0]) - c[1]) / 120.0, 0.0, 1.0);
        weight += w;
        sum += w * px;
      }
      require(weight > 20, "The far view model is seen");
      centroids.push_back(float(sum / weight) * float(kWidth) / image.width);
      std::printf("  %s camera at %.0f: centroid %.3f px\n", visibility, x, centroids.back());
    }
    eyeX = 0;
    float drift = 0;
    for (float c : centroids)
      drift = std::max(drift, std::abs(c - centroids[0]));
    std::printf("%s: view model centroid drift over camera positions up to 30518 units: %.3f px\n", visibility, drift);
    require(drift < .1f, "The view model stays on its pixels wherever the camera is");

    const float lit = luminance(worldShadow.at(litX, litY));
    const float shadowed = luminance(worldShadow.at(shadowX, centreY));
    const float passed = luminance(modelShadow.at(shadowX, centreY));
    std::printf("%s: wall lit %.0f, world quad's shadow %.0f, view model's place %.0f | centre: world quad behind "
                "the wall %.0f %.0f %.0f, view model %.0f %.0f %.0f\n",
                visibility, lit, shadowed, passed, worldBehind.at(centreX, centreY)[0],
                worldBehind.at(centreX, centreY)[1], worldBehind.at(centreX, centreY)[2],
                modelBehind.at(centreX, centreY)[0], modelBehind.at(centreX, centreY)[1],
                modelBehind.at(centreX, centreY)[2]);
    require(lit > 40, "The wall is lit");
    require(shadowed < lit * .5f, "Control: the world quad shadows the wall");
    require(passed > lit * .85f, "The view model casts no shadow on the world");
    require(red(modelShadow.at(centreX, centreY)), "The view model is seen (in front of the wall)");
    require(red(worldShadow.at(centreX, centreY)), "Control: the world quad in front of the wall is seen");
    require(!red(worldBehind.at(centreX, centreY)), "Control: the world quad behind the wall is hidden");
    // Behind the wall the sun does not reach it (the wall is between them):
    // seen, but dark. The scale keeps a game's view model out of walls.
    require(std::abs(luminance(modelBehind.at(centreX, centreY)) - luminance(worldBehind.at(centreX, centreY))) > 50,
            "The view model is seen where it reaches behind the wall");
    // Rasterized view-model parts nearer than the world camera's near plane
    // (a scope's mask and picture when aiming) are drawn: the near plane
    // moves in to the view model's.
    require(!red(worldNear.at(centreX, centreY)), "Control: the near plane clips the world quad");
    require(red(modelNear.at(centreX, centreY)), "A raster-only view model nearer than the near plane is drawn");
    // A cut view model (a scope's body around its lens): the camera sees
    // through the hole, the rest stays.
    require(!red(worldCut.at(centreX, centreY)) && red(worldCut.at(centreX + 40, centreY)),
            "Control: the camera sees through a world instance's cut");
    require(!red(modelCut.at(centreX, centreY)), "The camera sees through a view model's cut");
    require(red(modelCut.at(centreX + 40, centreY)), "The view model stays around its cut");
  }
  ok(api.DestroyMesh(wall));
  ok(api.DestroyMesh(occluder));
  ok(api.Shutdown());
  std::printf("view model test passed\n");
  return 0;
}
