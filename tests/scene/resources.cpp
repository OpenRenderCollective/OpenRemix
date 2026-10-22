#include "scene/fixtures.h"
#include "scene/images.h"
#include "scene/temporal.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <remix/remix_native_ext.h>

using namespace remix::test;
namespace {
  void cpuTests(const std::filesystem::path &directory) {
    remix::NativeResources r;
    auto red = material(r, 1, 1, 0, 0);
    auto mesh = quad(r, 2, red);
    auto draw = instance(mesh);
    for (auto category : {REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC,
                           REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC,
                           REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET,
                           REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET}) {
      auto decal = draw;
      decal.categoryFlags = category;
      r.drawInstance(decal);
      require(r.draws().back().decal, "Decal category was lost during scene submission");
      r.finishFrame();
    }
    r.drawInstance(draw);

    r.destroyMesh(mesh);
    r.destroyMaterial(red);
    require(r.pendingDrawCount() == 1, "Destroy discarded queued geometry");
    rejected([&] { r.drawInstance(draw); });
    r.finishFrame();
    require(!r.pendingDrawCount(), "Frame not consumed");
    red = material(r, 1, 1, 0, 0);
    mesh = quad(r, 2, red);
    require(reinterpret_cast<uintptr_t>(mesh) == 2, "Hash handle identity");
    remixapi_MeshInfo bad = {};
    bad.hash = 2;
    rejected([&] { r.createMesh(bad, mesh); });
    draw = instance(mesh);
    r.drawInstance(draw); // Failed replacement kept the original.
    draw.transform.matrix[0][0] = std::numeric_limits<float>::quiet_NaN();
    rejected([&] { r.drawInstance(draw); });
    // Mesh update hints retain immutable queued versions. Only exact topology
    // carries storage lineage; destruction, disabling, or index changes end it.
    {
      remixapi_HardcodedVertex v[3]{};
      v[1].position[0] = 1; v[2].position[1] = 1;
      v[0].normal[2] = 2;
      v[2].normal[0] = std::numeric_limits<float>::quiet_NaN();
      uint32_t indices[]{0,1,2};
      remixapi_MeshInfoSurfaceTriangles surface{};
      surface.vertices_values = v; surface.vertices_count = 3;
      surface.indices_values = indices; surface.indices_count = 3;
      remixapi_MeshInfoUpdateEXT update{};
      update.sType = REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT; update.enabled = 1;
      remixapi_MeshInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      info.hash = 900; info.pNext = &update; info.surfaces_count = 1; info.surfaces_values = &surface;
      remixapi_MeshHandle handle;
      auto submit = [&] { r.createMesh(info,handle); r.drawInstance(instance(handle)); return r.draws().back().mesh; };
      auto original = submit(); require(bool(original->updateStream), "Missing mesh update lineage");
      require(original->surfaces[0]->vertices[0].normal[2] == 1 &&
                  original->surfaces[0]->vertices[1].normal[2] == 0 &&
                  original->surfaces[0]->vertices[2].normal[0] == 0,
              "Authored normals were lost or legacy/invalid normals lost geometric fallback");
      v[0].position[0] = .01f; auto next = submit();
      require(original != next && original->updateStream == next->updateStream &&
              original->surfaces[0]->vertices[0].position[0] == 0, "Replacement mutated queued mesh or lost compatible lineage");
      update.enabled = 2; rejected([&] { r.createMesh(info,handle); });
      update.enabled = 1; require(submit() == next, "Invalid metadata changed current mesh");
      std::swap(indices[1],indices[2]); auto topology = submit();
      require(topology->updateStream != next->updateStream, "Index edit retained update lineage");
      r.destroyMesh(handle); auto recreated = submit();
      require(recreated->updateStream != topology->updateStream, "Destroyed hash retained update lineage");
      update.enabled = 0; require(!submit()->updateStream, "Disabled update retained lineage");
      remixapi_MeshInfoMotionEXT motion{};
      motion.sType = REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT; motion.enabled = 1;
      update.pNext = &motion;
      rejected([&] { r.createMesh(info,handle); }); // Motion needs storage lineage.
      update.enabled = 1; motion.enabled = 2;
      rejected([&] { r.createMesh(info,handle); });
      motion.enabled = 1;
      auto firstMotion = submit();
      auto previousDraw = r.draws().back(); previousDraw.instanceId = 71;
      remix::scene::TemporalHistory history;
      history.begin({1,1},1,480,270); history.stage(previousDraw); history.commit();
      history.begin({1,1},1,480,270);
      v[0].position[0] = .2f; auto moved = submit();
      auto movedDraw = r.draws().back(); movedDraw.instanceId = 71;
      require(moved != firstMotion && history.previousGeometry(movedDraw) &&
                  history.previousGeometry(movedDraw)->draw.surface->vertices[0].position[0] == .01f,
              "Explicit motion did not preserve last submitted immutable vertices");
      history.stage(movedDraw); // A discarded submission must not replace history.
      history.begin({1,1},1,480,270);
      require(history.previousGeometry(movedDraw)->draw.mesh == firstMotion,
              "Unsubmitted streamed update advanced history");
      motion.enabled = 0; submit();
      auto untracked = r.draws().back(); untracked.instanceId = 71;
      require(!history.previousGeometry(untracked), "Storage hint alone gained motion correspondence");
      motion.enabled = 1;
      submit(); auto reenabled = r.draws().back(); reenabled.instanceId = 71;
      require(!history.previousGeometry(reenabled), "Reenabled motion retained a disabled lifetime");
      std::swap(indices[1],indices[2]); submit();
      auto changedTopology = r.draws().back(); changedTopology.instanceId = 71;
      require(!history.previousGeometry(changedTopology), "Topology edit retained motion correspondence");
      r.destroyMesh(handle); submit();
      auto newLifetime = r.draws().back(); newLifetime.instanceId = 71;
      require(!history.previousGeometry(newLifetime), "Recreated hash retained motion correspondence");
      r.destroyMesh(handle); r.finishFrame();
    }
    remixapi_CameraInfoParameterizedEXT params = {};
    params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    params.aspect = 1;
    params.nearPlane = .1f;
    params.farPlane = 10;
    params.fovYInDegrees = 60;
    params.right = {1, 0, 0};
    params.up = {0, 1, 0};
    params.forward = {0, 0, 1};
    remixapi_CameraInfo cam = {};
    cam.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    cam.pNext = &params;
    cam.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
    r.setupCamera(cam);
    params.up = {1, 0, 0};
    rejected([&] { r.setupCamera(cam); });
    params.up = {0, 1, 0};
    params.fovYInDegrees = 180;
    rejected([&] { r.setupCamera(cam); });
    params.fovYInDegrees = 60;
    cam.type = REMIXAPI_CAMERA_TYPE_WORLD;
    remixapi_CameraTemporalEXT temporal{};
    temporal.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
    temporal.worldEpoch = 7;
    temporal.historyResetEpoch = 9;
    params.pNext = &temporal;
    remixapi_CameraEnvironmentEXT environment{};
    environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
    environment.enabled = 1;
    environment.radiance = {.2f, .4f, .8f};
    temporal.pNext = &environment;
    r.setupCamera(cam);
    const auto accepted = r.camera(), acceptedView = r.view(), acceptedProjection = r.projection();
    const auto acceptedDepth = r.reverseDepth();
    const auto acceptedSky = r.skyRadiance();
    require(acceptedSky == std::array<float, 3>{.2f, .4f, .8f}, "Linear sky was altered");
    params.position.x = 2;
    temporal.worldEpoch = 13;
    auto invalidEnvironment = [&] {
      rejected([&] { r.setupCamera(cam); });
      require(r.camera() == accepted && r.view() == acceptedView && r.projection() == acceptedProjection &&
                  r.reverseDepth() == acceptedDepth &&
                  r.skyRadiance() == acceptedSky &&
                  r.temporalEpoch().world == 7,
              "Invalid environment changed accepted camera state");
    };
    for (float bad : {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
      environment.radiance.y = bad;
      invalidEnvironment();
      environment.enabled = 0;
      invalidEnvironment(); // Disabled metadata must still be well-formed.
      environment.enabled = 1;
    }
    environment.radiance = {.2f, .4f, .8f};
    environment.enabled = 2;
    invalidEnvironment();
    environment.enabled = 0;
    for (auto type : {REMIXAPI_CAMERA_TYPE_SKY, REMIXAPI_CAMERA_TYPE_VIEW_MODEL}) {
      cam.type = type;
      r.setupCamera(cam);
      require(r.skyRadiance() == acceptedSky && r.temporalEpoch().world == 7,
              "Non-world camera replaced world environment or epoch");
    }
    cam.type = REMIXAPI_CAMERA_TYPE_WORLD;
    r.setupCamera(cam);
    require(r.skyRadiance() == std::array<float, 3>{}, "Disabled environment retained sky");
    environment.enabled = 1;
    r.setupCamera(cam);
    temporal.pNext = nullptr;
    r.setupCamera(cam);
    require(r.skyRadiance() == std::array<float, 3>{}, "Legacy camera retained sky");
    temporal.pNext = &environment;
    r.setupCamera(cam);
    r.reset();
    require(r.skyRadiance() == std::array<float, 3>{}, "Reset retained sky");
    require(r.reverseDepth() == std::array<float, 4>{0, 0, -1, 1}, "Reset retained camera depth");
    require(r.projection() == remix::scene::identity(), "Reset retained camera projection");
    // Editor cameras use a very wide clipping range at capture coordinates.
    // Translation and rotation must not corrupt the projection's far plane or
    // perspective W, which reconstruction uses to decide whether to stay active.
    for (bool parameterized : {false, true}) {
      remixapi_CameraInfo moving{};
      moving.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
      remixapi_CameraInfoParameterizedEXT p{};
      p.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
      p.aspect = 16.f / 9; p.nearPlane = .01f; p.farPlane = 100000; p.fovYInDegrees = 60;
      const float scale = 1 / std::tan(p.fovYInDegrees * .00872664625997165f);
      moving.projection[0][0] = scale / p.aspect;
      moving.projection[1][1] = scale;
      moving.projection[2][2] = p.farPlane / (p.farPlane - p.nearPlane);
      moving.projection[2][3] = 1;
      moving.projection[3][2] = -p.nearPlane * moving.projection[2][2];
      moving.pNext = parameterized ? &p : nullptr;
      auto expected = remix::scene::Matrix{};
      std::memcpy(expected.data(), moving.projection, sizeof(expected));
      expected[5] = -expected[5];
      for (unsigned frame = 0; frame < 128; ++frame) {
        const float angle = .01f * frame, c = std::cos(angle), s = std::sin(angle);
        p.position = {1024 + .3f * frame, -8192 + .7f * frame, 4096 - .1f * frame};
        p.right = {c, 0, -s}; p.up = {0, 1, 0}; p.forward = {s, 0, c};
        auto view = remix::scene::identity();
        view[0] = c; view[2] = s; view[8] = -s; view[10] = c;
        view[12] = -c * p.position.x + s * p.position.z;
        view[13] = -p.position.y;
        view[14] = -s * p.position.x - c * p.position.z;
        std::memcpy(moving.view, view.data(), sizeof(view));
        r.setupCamera(moving);
        require(r.projection() == expected, "Camera motion changed the submitted projection");
      }
      moving.type = REMIXAPI_CAMERA_TYPE_SKY;
      p.fovYInDegrees = 90; moving.projection[0][0] = .5f;
      r.setupCamera(moving);
      require(r.projection() == expected, "Sky camera replaced the world projection");
      require(r.projection(REMIXAPI_CAMERA_TYPE_SKY) != expected, "Sky projection was not stored separately");
    }
    // The private primary-depth transform must preserve clipping and retain a
    // millimeter overlay after a translated camera, for both camera APIs.
    for (bool parameterized : {false, true}) {
      remixapi_CameraInfo depthCamera{};
      depthCamera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
      remixapi_CameraInfoParameterizedEXT depthParameters{};
      depthParameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
      depthParameters.position = {0, 0, 4096};
      depthParameters.right = {1, 0, 0}; depthParameters.up = {0, 1, 0};
      depthParameters.forward = {0, 0, 1}; depthParameters.fovYInDegrees = 90;
      depthParameters.aspect = 1; depthParameters.nearPlane = .05f; depthParameters.farPlane = 512;
      for (int i = 0; i < 4; ++i) depthCamera.view[i][i] = 1;
      depthCamera.view[3][2] = -4096;
      depthCamera.projection[0][0] = depthCamera.projection[1][1] = 1;
      depthCamera.projection[2][2] = 512.f / 511.95f;
      depthCamera.projection[2][3] = 1;
      depthCamera.projection[3][2] = -.05f * depthCamera.projection[2][2];
      depthCamera.pNext = parameterized ? &depthParameters : nullptr;
      r.setupCamera(depthCamera);
      auto depthAt = [&](float distance) {
        const auto &row = r.reverseDepth();
        return (row[2] * (4096.f + distance) + row[3]) / distance;
      };
      require(std::abs(depthAt(.05f) - 1) < .002f && std::abs(depthAt(512)) < 1e-6f,
              "Reversed primary depth changed clipping planes");
      require(depthAt(64.f - .001f) > depthAt(64.f), "Reversed primary depth lost an overlay");
    }
    r.reset();
    remixapi_LightInfoDistantEXT distant{};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {0, -1, 0}; distant.angularDiameterDegrees = .533f;
    remixapi_LightInfo sunlight{};
    sunlight.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; sunlight.pNext = &distant;
    sunlight.hash = 789; sunlight.radiance = {1, 1, 1};
    remixapi_LightHandle sun{};
    r.createLight(sunlight, sun); r.drawLight(sun);
    const auto acceptedSun = r.light();
    for (float invalidDiameter : {-1.f, 180.f, std::numeric_limits<float>::infinity(),
                                   std::numeric_limits<float>::quiet_NaN()}) {
      distant.angularDiameterDegrees = invalidDiameter;
      rejected([&] { r.updateLight(sun, sunlight); });
      r.drawLight(sun);
      require(r.light() == acceptedSun, "Invalid sun diameter changed accepted light");
    }
    distant.angularDiameterDegrees = 0;
    r.updateLight(sun, sunlight); r.drawLight(sun);
    require(r.light().angularDiameterDegrees == 0, "Zero sun diameter did not retain hard shadows");
    remixapi_TextureInfo texture = {};
    texture.width = texture.height = 1;
    texture.mipLevels = 1;
    const uint8_t bgra[] = {10, 20, 30, 255};
    texture.data = bgra;
    texture.dataSize = 4;
    texture.format = REMIXAPI_FORMAT_B8G8R8A8_UNORM;
    auto decoded = remix::scene::decodeTexture(texture);
    require(decoded.mips[0] == std::vector<uint8_t>({30, 20, 10, 255}), "BGRA decoding");
    texture.mipLevels = 2;
    rejected([&] { remix::scene::decodeTexture(texture); });
    texture.mipLevels = 1;
    texture.dataSize = 3;
    rejected([&] { remix::scene::decodeTexture(texture); });
    const uint8_t bc1[] = {0, 0, 0, 0, 255, 255, 255, 255};
    texture.data = bc1;
    texture.dataSize = 8;
    texture.format = REMIXAPI_FORMAT_BC1_RGB_UNORM;
    require(remix::scene::decodeTexture(texture).mips[0][3] == 255, "Raw BC1 RGB is opaque");
    texture.format = REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT;
    require(remix::scene::decodeTexture(texture).mips[0][3] == 0, "Raw BC1 RGBA lost cutout alpha");
    texture.format = REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT;
    auto rgbaBc1 = remix::scene::decodeTexture(texture);
    require(rgbaBc1.srgb && rgbaBc1.mips[0][3] == 0, "sRGB BC1 changed alpha or transfer function");
    const float hdr[] = {8, .125f, 2, 1};
    texture.format = REMIXAPI_FORMAT_R32G32B32A32_SFLOAT_EXT;
    texture.data = hdr;
    texture.dataSize = sizeof(hdr);
    auto linearHdr = remix::scene::decodeTexture(texture);
    require(!linearHdr.srgb && linearHdr.hdrMips[0] == std::vector<float>({8, .125f, 2, 1}),
            "Immutable float upload preserves linear HDR radiance");
    texture.dataSize = sizeof(hdr) - 1;
    rejected([&] { remix::scene::decodeTexture(texture); });
    const uint16_t halfHdr[] = {0x4800, 0x3000, 0x4000, 0x3c00};
    texture.format = REMIXAPI_FORMAT_R16G16B16A16_SFLOAT_EXT;
    texture.data = halfHdr;
    texture.dataSize = sizeof(halfHdr);
    require(remix::scene::decodeTexture(texture).hdrMips == linearHdr.hdrMips,
            "Half and float HDR uploads agree");
    texture.data = nullptr;
    rejected([&] { remix::scene::decodeTexture(texture); });
    // Tiny valid DDS fixtures cover channel masks, BC1/2/3, odd dimensions and truncation.
    auto save = [&](const char *name, const std::vector<uint8_t> &data) {
      auto path = directory / name;
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char *>(data.data()), data.size());
      return path;
    };
    for (unsigned block = 1; block <= 3; ++block) {
      std::vector<uint8_t> dds(128 + (block == 1 ? 8 : 16), 0);
      auto word = [&](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
          dds[offset + i] = uint8_t(value >> (i * 8));
        }
      };
      word(0, 0x20534444);
      word(4, 124);
      word(12, 3);
      word(16, 3);
      word(28, 1);
      word(76, 32);
      word(80, 4);
      word(84, block == 1 ? 0x31545844 : block == 2 ? 0x33545844 : 0x35545844);
      const unsigned color = block == 1 ? 128 : 136;
      dds[color] = 0;
      dds[color + 1] = 0xf8;
      if (block == 2) {
        std::fill(dds.begin() + 128, dds.begin() + 136, 255);
      }
      if (block == 3) {
        dds[128] = 255;
        dds[129] = 255;
      }
      auto path = save("raster-texture.dds", dds);
      auto image = remix::scene::loadTexture(path);
      if (image.compressed) {
        image = remix::scene::decodeCompressed(image);
      }
      require(image.width == 3 && image.height == 3 && image.mips[0][0] == 255 && image.mips[0][3] == 255,
              "DDS block decode");
      dds.pop_back();
      save("raster-texture.dds", dds);
      rejected([&] { remix::scene::loadTexture(path); });
    }
    const auto png = remix::scene::loadTexture(directory.parent_path() / "raster-colors.png");
    require(png.width == 2 && png.height == 1 &&
                png.mips[0] == std::vector<uint8_t>({255, 0, 0, 255, 0, 255, 0, 128}),
            "PNG decoding");
    uint32_t pixel = 0xffffffff;
    r.registerUiTexture(77, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, &pixel, 4);
    ui(r, 77, 0xffffffff);
    r.freeUiTexture(77);
    rejected([&] { ui(r, 77, 0xffffffff); });
    remixapi_UIVertex uiVertices[3] = {};
    uint32_t uiIndices[] = {0, 1, 3};
    remixapi_UIDrawCommand uiCommand = {0, 3, 0, 0, 0};
    remixapi_UIDrawList list = {};
    list.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
    list.displayWidth = list.displayHeight = 64;
    list.pVertices = uiVertices;
    list.vertexCount = 3;
    list.pIndices = uiIndices;
    list.indexCount = 3;
    list.pCommands = &uiCommand;
    list.commandCount = 1;
    rejected([&] { r.submitUi(&list); });
    uiIndices[2] = 2;
    uiCommand.vertexOffset = -1;
    rejected([&] { r.submitUi(&list); });
    uiCommand.vertexOffset = 0;
    uiCommand.flags = 0x20;
    rejected([&] { r.submitUi(&list); });
    r.reset();
    require(!r.pendingDrawCount(), "Reset retained frame");
    std::puts("Raster resource validation, lifetime, cameras and DDS decoding passed");
  }

}
int main(int argc, char **argv) {
  require(argc == 2, "Supply scratch directory");
  const std::filesystem::path directory(argv[1]);
  std::filesystem::create_directories(directory);
  cpuTests(directory);
}
