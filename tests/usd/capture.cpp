// USD capture round trip: a frame drawn through the capture hooks is written
// in the Remix layout and read back by the loader with its hashes, transforms,
// categories, materials, textures, lights and camera.
#include "scene/images.h"
#include "scene/resources.h"
#include "usd/capture.h"
#include "usd/loader.h"
#include "usd/submission.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
using namespace remix;
namespace {
  void require(bool value, const char *message) {
    if (!value)
      throw std::runtime_error(message);
  }
#ifdef _WIN32
  constexpr remixapi_Path rgbaPath = L"0x00000000000000A1", bcPath = L"0x00000000000000B1",
                          normalPath = L"0x00000000000000C1";
#else
  constexpr remixapi_Path rgbaPath = "0x00000000000000A1", bcPath = "0x00000000000000B1",
                          normalPath = "0x00000000000000C1";
#endif
  constexpr uint64_t meshHash = 0x0123456789ABCDEFull, rgbaMaterial = 0x1111ull, bcMaterial = 0x2222ull,
                     lightHash = 0x3333ull, spotHash = 0x4444ull;
  const std::string spot = "IESNA:LM-63-2002\r\n"
                           "[TEST] spot\r\n"
                           "TILT=NONE\r\n"
                           "1 1000 1 3 2 1 1 0 0 0\r\n"
                           "1 1 10\r\n"
                           "0 45 90\r\n"
                           "0 180\r\n"
                           "120 60 0\r\n"
                           "80 40 0\r\n";
  void setEnvironment(const char *name, const std::string &value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
  }
} // namespace

int main() {
  const char *step = "setup";
  try {
    const auto directory =
        std::filesystem::temp_directory_path() / ("openremix-capture-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(directory);
    setEnvironment("OPENREMIX_CAPTURE_DIRECTORY", directory.string());
    scene::retainCompressedUploads = true;

    NativeResources resources;
    remixapi_TextureHandle texture{};
    const uint8_t rgba[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128};
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = 0xA1;
    info.width = info.height = 2;
    info.depth = info.mipLevels = 1;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.data = rgba;
    info.dataSize = sizeof rgba;
    step = "RGBA texture";
    resources.createTexture(info, texture);
    // One BC1 block: kept compressed, as "Keep BC textures compressed" does.
    const uint8_t block[8] = {0x00, 0xf8, 0x1f, 0x00, 0x55, 0x55, 0x55, 0x55};
    info.hash = 0xB1;
    info.width = info.height = 4;
    info.format = REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT;
    info.data = block;
    info.dataSize = sizeof block;
    step = "BC1 texture";
    resources.createTexture(info, scene::compressTexture(info), texture);
    // A tangent-space normal map: flat, and leaning fully toward +X.
    const uint8_t normals[2 * 4] = {128, 128, 255, 255, 255, 128, 128, 255};
    info.hash = 0xC1;
    info.width = 2;
    info.height = 1;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.data = normals;
    info.dataSize = sizeof normals;
    resources.createTexture(info, texture);

    remixapi_MaterialHandle materials[2]{};
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = .5f;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo material{};
    material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    material.pNext = &opaque;
    material.hash = rgbaMaterial;
    material.albedoTexture = rgbaPath;
    material.normalTexture = normalPath;
    material.wrapModeU = material.wrapModeV = 1;
    step = "material";
    resources.createMaterial(material, materials[0]);
    material.hash = bcMaterial;
    material.albedoTexture = bcPath;
    material.normalTexture = nullptr;
    material.emissiveIntensity = 2;
    material.emissiveColorConstant = {1, .5f, 0};
    resources.createMaterial(material, materials[1]);

    // Two surfaces: material subsets in the capture.
    remixapi_HardcodedVertex vertices[3] = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, 0xffffffffu},
                                            {{1, 0, 0}, {0, 0, 1}, {1, 0}, 0xffffffffu},
                                            {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0xffffffffu}};
    const uint32_t indices[3] = {0, 1, 2};
    remixapi_MeshInfoSurfaceTriangles surfaces[2]{};
    for (int i = 0; i < 2; ++i) {
      surfaces[i].vertices_values = vertices;
      surfaces[i].vertices_count = 3;
      surfaces[i].indices_values = indices;
      surfaces[i].indices_count = 3;
      surfaces[i].material = materials[i];
    }
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = meshHash;
    mesh.surfaces_values = surfaces;
    mesh.surfaces_count = 2;
    remixapi_MeshHandle meshHandle{};
    step = "mesh";
    resources.createMesh(mesh, meshHandle);

    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {0, 2, 0};
    sphere.radius = .25f;
    sphere.volumetricRadianceScale = 1;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &sphere;
    light.hash = lightHash;
    light.radiance = {10, 10, 10};
    remixapi_LightHandle lightHandle{};
    step = "light";
    resources.createLight(light, lightHandle);
    // A spot with an IES profile and a cookie (the RGBA texture).
    remixapi_LightInfoCookieEXT cookie{};
    cookie.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_COOKIE_EXT;
    cookie.pNext = &sphere;
    cookie.texture = rgbaPath;
    cookie.up = {0, 1, 0};
    cookie.fovDegrees = 40;
    remixapi_LightInfoIesProfileEXT ies{};
    ies.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_IES_PROFILE_EXT;
    ies.pNext = &cookie;
    ies.data = spot.data();
    ies.size = spot.size();
    light.pNext = &ies;
    light.hash = spotHash;
    remixapi_LightHandle spotHandle{};
    step = "spot light";
    resources.createLight(light, spotHandle);

    remixapi_CameraInfoParameterizedEXT parameters{};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.position = {0, 0, -5};
    parameters.forward = {0, 0, 1};
    parameters.up = {0, 1, 0};
    parameters.right = {1, 0, 0};
    parameters.fovYInDegrees = 60;
    parameters.aspect = 16.f / 9;
    parameters.nearPlane = .1f;
    parameters.farPlane = 1000;
    remixapi_CameraInfo camera{};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    camera.type = REMIXAPI_CAMERA_TYPE_WORLD;

    usd::setCaptureName("round trip: one");
    usd::requestCapture();
    usd::captureBoundary(resources); // the next frame is recorded
    require(usd::captureStatus().state == usd::CaptureStatus::State::Recording, "recording starts at a boundary");
    step = "camera";
    resources.setupCamera(camera);
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.mesh = meshHandle;
    instance.transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    usd::captureInstance(instance);
    instance.transform.matrix[0][3] = 4;
    instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
    usd::captureInstance(instance);
    usd::captureLight(lightHandle);
    usd::captureLight(spotHandle);
    usd::captureBoundary(resources);
    for (int i = 0; i < 600 && usd::captureStatus().state == usd::CaptureStatus::State::Writing; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      usd::captureBoundary(resources);
    }
    const auto status = usd::captureStatus();
    if (status.state != usd::CaptureStatus::State::Complete)
      throw std::runtime_error("capture failed: " + status.message);

    require(std::filesystem::path(status.path).filename() == "round trip_ one.usda", "chosen capture name");
    const auto capture = usd::load({status.path});
    const auto &s = capture.scene;
    // A mesh prim per surface, each with its own material binding.
    require(s["meshes"].size() == 4, "both instances composed, a prim per surface");
    bool moved = false, sky = false;
    for (const auto &item : s["meshes"]) {
      require(item["hash"].get<uint64_t>() == meshHash, "instance keeps the mesh hash");
      require(item["surfaces"].size() == 1, "one material per surface prim");
      moved |= std::abs(item["transform"][12].get<double>() - 4) < 1e-6;
      for (const auto &surface : item["surfaces"]) {
        sky |= (surface["category_flags"].get<uint32_t>() & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) != 0;
        const auto bound = surface["material"].get<std::string>();
        require(bound == "/RootNode/Looks/mat_0000000000001111" || bound == "/RootNode/Looks/mat_0000000000002222",
                "instances bind their surfaces' materials");
      }
    }
    require(moved, "instance transform");
    require(sky, "instance categories");
    bool emissive = false, block1 = false;
    for (const auto &item : s["materials"]) {
      const auto hash = item["hash"].get<uint64_t>();
      if (hash != rgbaMaterial && hash != bcMaterial)
        continue;
      const auto path = item["albedo_texture"].get<std::string>();
      require(!path.empty(), "albedo texture written");
      const auto pixels = scene::loadTexture(path);
      if (hash == bcMaterial) {
        emissive = item["emissive_intensity"].get<float>() > 0;
        block1 = pixels.compressed && pixels.compressed->upload.size() == sizeof block &&
                 std::equal(block, block + sizeof block, pixels.compressed->upload.begin());
      } else {
        // Normal maps are written as Remix samples them: unsigned octahedral.
        const auto normal = scene::loadTexture(item["normal_texture"].get<std::string>());
        const auto &n = normal.mips.at(0);
        require(item["normal_texture"].get<std::string>().ends_with("_OTH_Normal.dds") &&
                    item["normal_encoding"] == 0 && std::abs(n[0] - 128) <= 1 && std::abs(n[1] - 128) <= 1 &&
                    n[4] >= 250 && n[5] >= 250,
                "normal map converted to octahedral");
        require(pixels.width == 2 && pixels.mips.size() == 1 &&
                    std::equal(rgba, rgba + sizeof rgba, pixels.mips[0].begin()),
                "RGBA texture round trip");
      }
    }
    require(emissive, "material emission");
    require(block1, "BC blocks written verbatim");
    require(s["lights"].size() == 2, "both lights");
    for (const auto &item : s["lights"]) {
      require(item["type"] == "SphereLight", "sphere lights");
      const auto hash = item["hash"].get<uint64_t>();
      require(hash == lightHash || hash == spotHash, "lights keep their hashes");
      const auto ies = item["ies"].get<std::string>(), texture = item["cookie"].get<std::string>();
      if (hash == lightHash) {
        require(ies.empty() && texture.empty(), "plain light has no profile or cookie");
        continue;
      }
      std::ifstream input(ies, std::ios::binary);
      require(std::string(std::istreambuf_iterator<char>(input), {}) == spot, "IES profile written verbatim");
      require(!texture.empty() && item["cookie_fov"].get<float>() == 40 && item["cookie_up"][1] == 1,
              "cookie texture, up and field of view");
    }
    // The loaded scene recreates both, with the profile and cookie attached.
    {
      NativeResources replay;
      usd::Submission submission(replay, capture, 1);
      submission.draw(false, 1);
      bool restored = false;
      for (const auto &light : replay.sphereLights())
        restored |= light.ies != 0 && light.cookie != 0;
      require(restored, "IES profile and cookie restored on load");
    }
    require(s["cameras"].size() == 1, "camera");
    bool replaceable = false;
    for (const auto &entry : s["replacements"]["meshes"])
      replaceable |= entry["hash"].get<uint64_t>() == meshHash;
    require(replaceable, "captured mesh is a replacement target");

    std::filesystem::remove_all(directory);
    std::cout << "USD capture round trip passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  } catch (remixapi_ErrorCode code) {
    std::cerr << step << ": error code " << code << "\n";
    return 1;
  }
}
