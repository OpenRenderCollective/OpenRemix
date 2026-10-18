// Metal backend raster overlays through the public API: halo outlines
// (remixapi_InstanceInfoHaloEXT), masked overlays and role CUT
// (remixapi_InstanceInfoMaskEXT) and soft sprite overlays over a traced scene
// (remixapi_InstanceInfoSpriteEXT, as tests/pathtrace/soft_sprites.cpp). The
// camera looks down +z; quads span x0..x1 of the view's width (-1..1).
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <remix/remix_reconstruction_ext.h>
#include <dlfcn.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
  remixapi_Interface api = {};
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void checked(remixapi_ErrorCode code, int line) {
    if (code) {
      std::fprintf(stderr, "Unexpected error: 0x%x at line %d\n", unsigned(code), line);
      std::exit(1);
    }
  }
#define ok(code) checked((code), __LINE__)

  struct Image {
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    // Mean of a channel over an 8x8 block centred at (x, y) of a 256x128
    // view (scaled to the drawable: a Retina window has more pixels), 0..1.
    float channel(int x, int y, int c) const {
      const int scale = width / 256;
      float sum = 0;
      int count = 0;
      for (int py = (y - 4) * scale; py < (y + 4) * scale; ++py)
        for (int px = (x - 4) * scale; px < (x + 4) * scale; ++px, ++count)
          sum += pixels[(size_t(py) * width + px) * 4 + c] / 255.f;
      return sum / float(count);
    }
  };

  remixapi_MaterialHandle material(uint64_t hash, remixapi_Float3D emissive = {}) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    opaque.useDrawCallAlphaState = 1;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = hash;
    info.emissiveColorConstant = emissive;
    info.emissiveIntensity = emissive.x + emissive.y + emissive.z > 0 ? 1.f : 0.f;
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  }

  // A quad facing the camera at depth z, over x0..x1 of the view's width and
  // the middle `height` of its height.
  remixapi_MeshHandle quad(uint64_t hash, remixapi_MaterialHandle material, float z, float x0, float x1,
                           uint32_t color, float height = .5f) {
    remixapi_HardcodedVertex vertices[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      vertices[i].position[0] = (i == 0 || i == 3 ? x0 : x1) * 2 * z;
      vertices[i].position[1] = (i < 2 ? -height : height) * z;
      vertices[i].position[2] = z;
      vertices[i].normal[2] = -1;
      vertices[i].color = color;
    }
    uint32_t indices[] = {0, 2, 1, 0, 3, 2};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = material;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle result{};
    ok(api.CreateMesh(&info, &result));
    return result;
  }

  void setCamera() {
    remixapi_CameraInfoParameterizedEXT parameters{};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.right = {1, 0, 0};
    parameters.up = {0, 1, 0};
    parameters.forward = {0, 0, 1};
    parameters.aspect = 2;
    parameters.fovYInDegrees = 90;
    parameters.nearPlane = .1f;
    parameters.farPlane = 100;
    remixapi_CameraInfo camera{};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    ok(api.SetupCamera(&camera));
  }

  void draw(remixapi_MeshHandle mesh, const void *extension = nullptr) {
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = const_cast<void *>(extension);
    instance.mesh = mesh;
    instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i)
      instance.transform.matrix[i][i] = 1;
    ok(api.DrawInstance(&instance));
  }

  // Presents frames of scene and returns the screenshot of the last one.
  Image capture(const std::function<void()> &scene, unsigned frames, const std::string &path) {
    std::filesystem::remove(path);
    const auto frame = [&] {
      ok(api.PumpEvents());
      setCamera();
      scene();
      ok(api.Present(nullptr));
    };
    for (unsigned i = 0; i + 1 < frames; ++i)
      frame();
    ok(api.RequestPresentedScreenshot(path.c_str()));
    frame();
    // The capture is written when the next presentation begins.
    frame();
    int channels = 0;
    Image image;
    unsigned char *data = stbi_load(path.c_str(), &image.width, &image.height, &channels, 4);
    require(data && image.width >= 256 && image.width % 256 == 0 && image.width == 2 * image.height, "Overlay capture unavailable");
    image.pixels.assign(data, data + size_t(image.width) * image.height * 4);
    stbi_image_free(data);
    return image;
  }
} // namespace

int main(int argc, char **argv) {
  require(argc == 3, "usage: remix-metal-overlays-test LIBRARY OUTPUT_DIRECTORY");
  const std::string directory = std::filesystem::absolute(argv[2]).string();
  std::filesystem::create_directories(directory);
  remixapi_HMODULE library = nullptr;
  ok(remixapi_lib_loadRemixDllAndInitialize(argv[1], &api, &library));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.windowTitle = "Remix Metal overlays test";
  startup.windowWidth = 256;
  startup.windowHeight = 128;
  ok(api.Startup(&startup));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  // The default profile may path trace (runtime/settings.cpp): the raster
  // checks come first.
  ok(api.SetConfigVariable("rtx.native.backend", "raster"));
  const auto white = material(1);

  // Halo outlines (raster backend): a red glow around a small quad in front
  // of a grey wall, nothing inside the silhouette or far from it.
  {
    const auto wall = quad(2, white, 3, -1, 1, 0xff404040, 1);
    const auto source = quad(3, white, 2, -.1f, .1f, 0xffffffff);
    remixapi_InstanceInfoHaloEXT halo{};
    halo.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_HALO_EXT;
    halo.color = {1, 0, 0};
    halo.blurX = halo.blurY = 32;
    halo.passes = 1;
    halo.additive = 1;
    const auto image = capture(
        [&] {
          draw(wall);
          draw(source, &halo);
        },
        4, directory + "/halo.png");
    const float inside = image.channel(128, 64, 0), near = image.channel(148, 64, 0), far = image.channel(16, 64, 0);
    const float grey = image.channel(16, 64, 1);
    std::printf("halo: red inside %.3f, beside %.3f, far %.3f (green %.3f)\n", inside, near, far, grey);
    require(std::abs(inside - grey) < .03f, "Halo: the silhouette is not left out");
    require(near > grey + .1f, "Halo: no glow beside the silhouette");
    require(std::abs(far - grey) < .03f, "Halo: glow far from the silhouette");
  }

  // Masked overlays (raster backend): group 1 covers the left half; a blue
  // overlay is drawn inside it, a green one outside it.
  {
    const auto wall = quad(4, white, 3, -1, 1, 0xff404040, 1);
    const auto source = quad(5, white, 2.5f, -1, 0, 0xffffffff, 1);
    const auto blue = quad(6, white, 2, -1, 1, 0xff0000ff, .3f);
    const auto green = quad(7, white, 2, -1, 1, 0xff00ff00, .1f);
    remixapi_InstanceInfoMaskEXT sourceMask{};
    sourceMask.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT;
    sourceMask.group = 1;
    sourceMask.role = REMIXAPI_INSTANCE_MASK_SOURCE_EXT;
    auto inside = sourceMask, outside = sourceMask;
    inside.role = REMIXAPI_INSTANCE_MASK_INSIDE_EXT;
    outside.role = REMIXAPI_INSTANCE_MASK_OUTSIDE_EXT;
    const auto image = capture(
        [&] {
          draw(wall);
          draw(blue, &inside);
          draw(green, &outside);
          draw(source, &sourceMask);
        },
        4, directory + "/mask.png");
    // Rows: 64 is inside both overlays (green over blue), 52 only the blue one.
    const float leftBlue = image.channel(64, 52, 2), rightBlue = image.channel(192, 52, 2);
    const float leftGreen = image.channel(64, 64, 1), rightGreen = image.channel(192, 64, 1);
    std::printf("mask: blue left %.3f right %.3f, green left %.3f right %.3f\n", leftBlue, rightBlue, leftGreen,
                rightGreen);
    require(leftBlue > rightBlue + .3f, "Mask: the INSIDE overlay is not limited to the coverage");
    require(rightGreen > leftGreen + .3f, "Mask: the OUTSIDE overlay is not limited to outside the coverage");
  }

  // Path tracing from here.
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  for (const char *key : {"prism", "gamma22", "sharc", "sigma", "taa", "bounces", "accumulate"})
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.denoiser", "off"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "off"));
  const auto capabilitiesOf = reinterpret_cast<PFN_remixapi_GetBackendCapabilitiesEXT>(
      dlsym(library, "remixapi_GetBackendCapabilitiesEXT"));
  require(capabilitiesOf, "Backend capabilities are not exported");

  // Role CUT: an emissive red quad in front of an emissive green wall is not
  // seen by the camera where group 1 (the left half) covers.
  {
    const auto red = material(8, {1, 0, 0}), green = material(9, {0, 1, 0});
    const auto wall = quad(10, green, 3, -1, 1, 0xffffffff, 1);
    const auto front = quad(11, red, 2, -1, 1, 0xffffffff, 1);
    const auto source = quad(12, white, 1.5f, -1, 0, 0xffffffff, 1);
    remixapi_InstanceInfoMaskEXT sourceMask{};
    sourceMask.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT;
    sourceMask.group = 1;
    auto cut = sourceMask;
    cut.role = REMIXAPI_INSTANCE_MASK_CUT_EXT;
    const auto scene = [&] {
      draw(wall);
      draw(front, &cut);
      draw(source, &sourceMask);
    };
    remixapi_BackendCapabilitiesEXT capabilities{.structSize = sizeof(capabilities)};
    for (unsigned frame = 0; frame < 200000 && capabilities.activeBackend != 1; ++frame) {
      ok(api.PumpEvents());
      setCamera();
      scene();
      ok(api.Present(nullptr));
      ok(capabilitiesOf(&capabilities));
    }
    require(capabilities.activeBackend == 1, "Path tracing did not activate");
    const auto image = capture(scene, 8, directory + "/cut.png");
    const float leftRed = image.channel(64, 64, 0), leftGreen = image.channel(64, 64, 1);
    const float rightRed = image.channel(192, 64, 0), rightGreen = image.channel(192, 64, 1);
    std::printf("cut: left red %.3f green %.3f, right red %.3f green %.3f\n", leftRed, leftGreen, rightRed,
                rightGreen);
    require(leftGreen > leftRed + .2f, "Cut: the traced instance is seen inside the coverage");
    require(rightRed > rightGreen + .2f, "Cut: the traced instance is missing outside the coverage");
  }

  // Soft sprite overlays over the traced scene (tests/pathtrace/soft_sprites.cpp).
  {
    const auto wall = quad(20, white, 3, -1, 1, 0xffffffff, 1);
    const auto half = quad(21, white, 2.8f, -.9f, -.6f, 0xff00ff00);
    const auto reference = quad(22, white, 2.8f, -.4f, -.1f, 0x8000ff00);
    const auto whole = quad(23, white, 2.8f, .1f, .4f, 0xff00ff00);
    const auto open = quad(24, white, 2.8f, .6f, .9f, 0xff00ff00);
    // Then upscaled by MetalFX: the depth the sprites fade against is the
    // composite's, at output resolution (the stable depth pass).
    for (const char *primary : {"ray", "raster", "metalfx"}) {
      const bool upscaled = std::string(primary) == "metalfx";
      ok(api.SetConfigVariable("rtx.native.pathtrace.primaryVisibility", upscaled ? "ray" : primary));
      if (upscaled) {
        ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "auto"));
        ok(api.SetConfigVariable("rtx.native.pathtrace.dlssQuality", "quality"));
        // The configuration with MetalFX is prepared in the background.
        const auto diagnosticsOf = reinterpret_cast<remixapi_ErrorCode(REMIXAPI_CALL *)(
            remixapi_ReconstructionDiagnosticsEXT *)>(dlsym(library, "remixapi_GetReconstructionDiagnosticsEXT"));
        require(diagnosticsOf, "Reconstruction diagnostics are not exported");
        remixapi_ReconstructionDiagnosticsEXT diagnostics{};
        for (unsigned frame = 0; frame < 200000; ++frame) {
          diagnostics = {};
          diagnostics.structSize = sizeof(diagnostics);
          ok(diagnosticsOf(&diagnostics));
          if (diagnostics.activeBackend)
            break;
          ok(api.PumpEvents());
          setCamera();
          ok(api.Present(nullptr));
        }
        require(diagnostics.activeBackend != 0, "MetalFX reconstruction did not activate");
      }
      for (bool soft : {true, false}) {
        const auto image = capture(
            [&] {
              draw(wall);
              const struct {
                remixapi_MeshHandle mesh;
                float softDistance;
              } sprites[] = {{half, .4f}, {reference, 0}, {whole, .05f}, {open, .4f}};
              for (const auto &entry : sprites) {
                remixapi_InstanceInfoBlendEXT blend{};
                blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
                blend.alphaTestCompareOp = 7;
                blend.alphaBlendEnabled = 1;
                blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
                blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
                blend.writeMask = 15;
                blend.textureColorOperation = 3, blend.textureColorArg1Source = 1, blend.textureColorArg2Source = 2;
                blend.textureAlphaOperation = 3, blend.textureAlphaArg1Source = 1, blend.textureAlphaArg2Source = 2;
                remixapi_InstanceIdentityEXT identity{};
                identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
                identity.pNext = &blend;
                identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
                identity.rasterVisible = 1;
                remixapi_InstanceInfoSpriteEXT sprite{};
                sprite.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT;
                sprite.pNext = &identity;
                sprite.flags = REMIXAPI_SPRITE_SOFT_DEPTH_EXT;
                sprite.softDistance = entry.softDistance;
                draw(entry.mesh, entry.softDistance > 0 && soft ? static_cast<const void *>(&sprite)
                                                                : static_cast<const void *>(&identity));
              }
            },
            upscaled ? 48 : 8, directory + "/sprites-" + primary + (soft ? "-soft.png" : "-hard.png"));
        std::array<float, 4> green{};
        for (unsigned sample = 0; sample < 4; ++sample)
          green[sample] = image.channel(int(sample * 64 + 32), 64, 1);
        std::printf("sprites %s %s: half way %.3f, reference %.3f, beyond %.3f, last %.3f\n", primary,
                    soft ? "soft" : "hard", green[0], green[1], green[2], green[3]);
        if (soft) {
          require(green[1] > .2f && green[1] < .8f, "Sprites: the reference is not half transparent");
          require(std::abs(green[0] - green[1]) < .03f, "Sprites: half way into its distance is not half transparent");
          require(green[2] > .97f, "Sprites: beyond its distance is not opaque");
        } else
          require(green[0] > .97f && green[2] > .97f && green[3] > .97f, "Sprites: plain sprites are not opaque");
      }
    }
  }
  ok(api.Shutdown());
  std::puts("Metal raster overlays passed");
  return 0;
}
