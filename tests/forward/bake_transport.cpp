#include "forward/bake/transport.h"
#include "forward/lightmap/layout.h"
#include "gpu_fixture.h"
#include "probe_queries.h"
#include "raster/renderer.h"
#include "runtime/pipelines.h"
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

using namespace remix;
using namespace remix::forward;
using remix::forward::test::require;
namespace bake = remix::forward::bake;
namespace {
  struct MaterialSettings {
    float diffuse = 0, emission = 0, metallic = 0, opacity = 1;
    remixapi_Path albedo = nullptr, emissive = nullptr;
    bool cutout = false, suppress = false, self = false;
  };
  struct World {
    RasterRenderer resources;
    uint64_t hash = 1000, revision = 0;
    World(test::Fixture &fixture) { resources.initializeGpu(fixture.physical, fixture.device); }
    remixapi_MaterialHandle material(const MaterialSettings &settings) {
      remixapi_MaterialInfoSelfIlluminationEXT self{};
      self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
      self.maskScale = 1;
      remixapi_MaterialInfoEmissionEXT policy{};
      policy.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT;
      policy.suppressLightGeneration = settings.suppress;
      policy.pNext = settings.self ? &self : nullptr;
      remixapi_MaterialInfoOpaqueEXT opaque{};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.pNext = &policy;
      opaque.albedoConstant = {settings.diffuse, settings.diffuse, settings.diffuse};
      opaque.opacityConstant = settings.opacity;
      opaque.roughnessConstant = .8f;
      opaque.metallicConstant = settings.metallic;
      opaque.alphaTestType = settings.cutout ? 4 : 7;
      opaque.alphaReferenceValue = 128;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &opaque;
      info.hash = ++hash;
      info.albedoTexture = settings.albedo;
      info.emissiveTexture = settings.emissive;
      info.emissiveIntensity = settings.emission;
      info.emissiveColorConstant = settings.emissive ? remixapi_Float3D{1, 1, 1} : remixapi_Float3D{1, 0, 0};
      remixapi_MaterialHandle result;
      resources.createMaterial(info, result);
      return result;
    }
    uint64_t quad(remixapi_MaterialHandle material, const std::array<Vec3, 4> &positions,
                  remixapi_InstanceInfoBlendEXT *blend = nullptr, uint32_t color = 0xffffffff,
                  Vec3 normal = {}) {
      remixapi_HardcodedVertex vertices[4]{};
      for (uint32_t i = 0; i < 4; ++i) {
        std::copy_n(positions[i].begin(), 3, vertices[i].position);
        vertices[i].texcoord[0] = i == 1 || i == 2 ? 1 : 0;
        vertices[i].texcoord[1] = i >= 2 ? 1 : 0;
        vertices[i].color = color;
        std::copy_n(normal.begin(), 3, vertices[i].normal);
      }
      const uint32_t indices[] = {0, 2, 1, 0, 3, 2};
      remixapi_MeshInfoSurfaceTriangles surface{};
      surface.vertices_values = vertices;
      surface.vertices_count = 4;
      surface.indices_values = indices;
      surface.indices_count = 6;
      surface.material = material;
      remixapi_MeshInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      info.hash = ++hash;
      info.surfaces_values = &surface;
      info.surfaces_count = 1;
      remixapi_MeshHandle mesh;
      resources.createMesh(info, mesh);
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.mesh = mesh;
      instance.pNext = blend;
      instance.doubleSided = 1;
      instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
      return resources.createRetained(instance);
    }
    uint64_t wall(remixapi_MaterialHandle material, float z, float size,
                  remixapi_InstanceInfoBlendEXT *blend = nullptr, uint32_t color = 0xffffffff) {
      return quad(material, {{{-size, -size, z}, {size, -size, z}, {size, size, z}, {-size, size, z}}}, blend,
                  color);
    }
    void texture(uint64_t id, const uint8_t (&pixels)[8],
                 remixapi_Format format = REMIXAPI_FORMAT_R8G8B8A8_UNORM) {
      remixapi_TextureInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      info.hash = id;
      info.width = 2;
      info.height = info.depth = info.mipLevels = 1;
      info.format = format;
      info.data = pixels;
      info.dataSize = sizeof(pixels);
      remixapi_TextureHandle texture;
      resources.createTexture(info, texture);
    }
    std::shared_ptr<scene::BakeScene> scene(std::initializer_list<uint64_t> instances,
                                            std::initializer_list<remixapi_LightHandle> lights = {}) {
      remixapi_BakeSceneInfoEXT info{};
      info.structSize = sizeof(info);
      info.version = REMIXAPI_BAKE_SCENE_VERSION_EXT;
      info.sceneId = 1;
      info.revision = ++revision;
      info.sceneScale = 1;
      info.state = REMIXAPI_BAKE_SCENE_READY_EXT;
      info.instances = instances.begin();
      info.instanceCount = uint32_t(instances.size());
      info.lights = lights.begin();
      info.lightCount = uint32_t(lights.size());
      resources.setBakeScene(&info);
      return std::make_shared<scene::BakeScene>(*resources.bakeScene());
    }
  };
  float evaluate(const bake::Result &value, Vec3 n, uint32_t channel) {
    const float sh[] = {.2820947918f,
                        .4886025119f * n[1],
                        .4886025119f * n[2],
                        .4886025119f * n[0],
                        1.092548431f * n[0] * n[1],
                        1.092548431f * n[1] * n[2],
                        .3153915653f * (3 * n[2] * n[2] - 1),
                        1.092548431f * n[0] * n[2],
                        .5462742153f * (n[0] * n[0] - n[1] * n[1])};
    float result = 0;
    for (uint32_t i = 0; i < 9; ++i)
      result += value.irradiance[i][channel] * sh[i];
    return result;
  }
  bake::Result run(test::Fixture &fixture, pipeline::Service &pipelines, World &world,
                   std::shared_ptr<scene::BakeScene> scene, uint32_t bounces = 3,
                   uint32_t samples = 1024, bool parallel = true) {
    bake::Transport transport;
    bake::Options options;
    options.textureResolution = 32;
    options.batchPoints = 4;
    options.rayBudget = 64u * 1024;
    options.parallel = parallel;
    static uint64_t transportGeneration = 0;
    auto job = pipelines.prepare("bake-test-" + std::to_string(++transportGeneration), [&] {
      transport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Bake preparation worker timed out");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    const auto before = transport.memoryBytes();
    transport.releaseBuildInputs();
    require(transport.memoryBytes() < before,
            "Bake releases acceleration build storage after its preparation fence");
    bool refused = false;
    const bake::Point excessive[4]{};
    try {
      transport.recordBatch(fixture.command, 0, excessive, 8192, 8);
    } catch (const std::invalid_argument &) {
      refused = true;
    }
    require(refused, "Excessive bake dispatch work is refused before command recording");
    const bake::Point points[] = {{}, {{.1f, 0, 0, 0}, {}}};
    bake::Result results[2];
    std::array<bake::Result, 2> reference;
    for (uint32_t slot = 0; slot < gpuFrameCount; ++slot) {
      fixture.begin();
      transport.recordBatch(fixture.command, slot, points, samples, bounces);
      fixture.finish();
      transport.read(slot, results);
      for (uint32_t i = 0; i < 2; ++i) {
        if (slot == 0)
          reference[i] = results[i];
        else
          require(results[i].irradiance == reference[i].irradiance &&
                      results[i].moments == reference[i].moments &&
                      results[i].statistics == reference[i].statistics,
                  "Identical transport inputs produce identical results across frame slots");
      }
      require(results[0].statistics[2] == samples && results[1].statistics[2] == samples,
              "Every frame slot records a batch of independent point results");
      for (const auto &coefficient : results[0].irradiance)
        for (float value : coefficient)
          require(std::isfinite(value), "Finite baked irradiance coefficients");
    }
    const bake::Point distant{{0, 0, -1000, 0}, {}};
    fixture.begin();
    transport.recordBatch(fixture.command, 0, {&distant, 1}, 256, bounces, 1024);
    fixture.finish();
    bake::Result reused;
    transport.read(0, {&reused, 1});
    require(reused.statistics[1] == 1 && reused.statistics[2] == 256,
            "Reused slots replace points, counts and sampling offsets");
    return results[0];
  }
  void liveLighting(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto wall = world.wall(world.material({.diffuse = 1}), 1, 2);
    auto source = world.scene({wall});
    source->sun.isDistant = false;
    source->dome.enabled = false;
    bake::Transport transport;
    bake::Options options;
    options.batchPoints = 1;
    options.textureResolution = 16;
    auto job = pipelines.prepare("live-transport-lighting", [&] {
      transport.initialize(fixture.physical, fixture.device, source, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Live lighting transport prepared");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    transport.releaseBuildInputs();
    LightData light;
    light.positionRadius = {0, 0, -1, .25f};
    light.radiance = {100, 0, 0, 0};
    light.directionCone = {0, 0, -1, -1};
    const bake::Point ray{{0, 0, 0, 0}, {0, 0, 1, 0}};
    auto sample = [&](uint32_t slot, std::span<const LightData> lights) {
      fixture.begin();
      transport.updateLighting(slot, lights, source->sun, source->dome, 10);
      transport.recordRadiance(fixture.command, slot, {&ray, 1}, 1);
      fixture.finish();
      bake::Result result;
      transport.read(slot, {&result, 1});
      return result.irradiance[0];
    };
    for (uint32_t slot = 0; slot < gpuFrameCount; ++slot) {
      const auto red = sample(slot, {&light, 1});
      require(red[0] > .01f && red[1] == 0 && red[2] == 0, "Live analytic lighting reaches ray-hit shading");
    }
    light.radiance = {0, 0, 100, 0};
    const auto blue = sample(0, {&light, 1});
    require(blue[2] > .01f && blue[0] == 0,
            "A completed slot streams changed light colour without a scene rebuild");
    std::vector<LightData> many(65, light);
    const auto resized = sample(0, many);
    require(resized[2] > .01f && resized[0] == 0,
            "A completed slot can replace its live light buffer safely");
    light.positionRadius[2] = 2;
    const auto moved = sample(0, {&light, 1});
    require(moved[2] < .001f, "Light placement updates the current material-facing visibility");
    const auto removed = sample(0, {});
    require(removed[0] == 0 && removed[1] == 0 && removed[2] == 0,
            "Removing streamed lights clears their contribution without stale "
            "captured illumination");
  }
  void bakeVisibility(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto receiver = world.wall(world.material({}), 0, 2);
    const auto blocker =
        world.quad(world.material({}), {{{-3, -3, -.5f}, {0, -3, -.5f}, {0, 3, -.5f}, {-3, 3, -.5f}}});
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {0, 0, -1.5f};
    sphere.radius = .25f;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = ++world.hash;
    info.radiance = {2, 3, 4};
    remixapi_LightHandle light;
    world.resources.createLight(info, light);
    auto source = world.scene({receiver, blocker}, {light});
    source->sun.isDistant = true;
    source->sun.direction[0] = source->sun.direction[1] = 0;
    source->sun.direction[2] = 1;
    source->sun.angularDiameterDegrees = 30;
    bake::Transport transport;
    bake::Options options;
    options.textureResolution = 32;
    options.batchPoints = 8;
    options.initialBatchPoints = 2;
    options.queueFamily = fixture.family;
    auto job = pipelines.prepare("mixed-visibility-test", [&] {
      transport.initialize(fixture.physical, fixture.device, source, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Visibility transport preparation completed");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    transport.releaseBuildInputs();
    const bake::VisibilityPoint points[] = {{{{-.6f, 0, 0, 0}, {0, 0, -1, 0}}, 0},
                                            {{{0, 0, 0, 0}, {0, 0, -1, 0}}, 0},
                                            {{{.6f, 0, 0, 0}, {0, 0, -1, 0}}, 0}};
    fixture.begin();
    transport.recordVisibility(fixture.command, 0, points, 1024);
    fixture.finish();
    bake::Result results[3];
    transport.read(0, results);
    require(transport.timingSupported() && transport.elapsed(0).value_or(0) > 0,
            "A completed batch exposes nonblocking GPU duration after growing its slot buffers");
    const float left = results[0].irradiance[0][0], edge = results[1].irradiance[0][0],
                right = results[2].irradiance[0][0];
    require(left < .05f && edge > .25f && edge < .75f && right > .95f,
            "Hardware emitter sampling resolves static umbra, soft penumbra and "
            "full visibility separately");
    require(results[1].statistics[2] == 1024, "Per-light visibility retains its independent sample budget");
    require(right == 1,
            "Fully unobstructed transport preserves an exact clear endpoint despite weighted reduction");
    for (uint32_t samples : {1u, 63u, 65u}) {
      fixture.begin();
      transport.recordVisibility(fixture.command, 0, points, samples, true);
      fixture.finish();
      transport.read(0, results);
      require(results[0].irradiance[0][0] == 0 && results[2].irradiance[0][0] == 1 &&
                  results[2].statistics[2] == samples,
              "Sample reduction preserves exact visibility and counts for partially occupied workgroups");
    }
    fixture.begin();
    transport.recordVisibility(fixture.command, 0, points, 1024, true);
    fixture.finish();
    transport.read(0, results);
    const float direct = evaluate(results[2], {0, 0, -1}, 1);
    require(results[2].irradiance[0][0] == 1,
            "Direct SH integration preserves exact clear visibility independently of its coefficients");
    const float expected =
        3.14159265f * .25f * .25f / (1.5f * 1.5f + .6f * .6f) * (1.5f / std::sqrt(1.5f * 1.5f + .6f * .6f));
    require(evaluate(results[0], {0, 0, -1}, 1) == 0 && direct > expected * .88f && direct < expected * 1.12f,
            "Unit-radiance direct SH preserves static occlusion and approximates "
            "the analytic sphere irradiance");
    require(direct > 8 * std::max(evaluate(results[2], {0, 0, 1}, 1), .00001f) &&
                results[1].irradiance[0][0] > .25f && results[1].irradiance[0][0] < .75f,
            "Direct diffuse keeps directional normal response and a separate "
            "soft specular visibility mask");
    std::printf("Unit direct irradiance: %.5f, analytic %.5f\n", direct, expected);
    auto invalid = points[0];
    invalid.light = 1;
    bool rejected = false;
    try {
      transport.recordVisibility(fixture.command, 0, {&invalid, 1});
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected, "Invalid captured light indices are refused before command recording");
    std::printf("Static emitter visibility: umbra %.3f, penumbra %.3f, exposed %.3f\n", left, edge, right);
    const bake::Point sunPoints[] = {points[0].surface, points[1].surface, points[2].surface};
    fixture.begin();
    transport.recordSun(fixture.command, 0, sunPoints, 1024, true);
    fixture.finish();
    transport.read(0, results);
    require(results[0].irradiance[0][0] == 0 && results[1].irradiance[0][0] > .3f &&
                results[1].irradiance[0][0] < .7f && results[2].irradiance[0][0] == 1 &&
                results[2].statistics[2] == 1024,
            "Sun disc visibility distinguishes umbra, soft penumbra and exact clear transport");
    const float sunDiffuse = evaluate(results[2], {0, 0, -1}, 1);
    require(sunDiffuse > .9f && sunDiffuse < 1.12f &&
                evaluate(results[0], {0, 0, -1}, 1) == 0 &&
                sunDiffuse > 8 * std::max(evaluate(results[2], {0, 0, 1}, 1), .00001f),
            "Unit sunlight SH preserves visibility and directional normal response even with black captured radiance");
    std::printf("Sun visibility: %.3f %.3f %.3f; unit direct %.5f\n", results[0].irradiance[0][0],
                  results[1].irradiance[0][0], results[2].irradiance[0][0], sunDiffuse);
    auto triangleSource = std::make_shared<scene::BakeScene>(*source);
    auto &emitter = triangleSource->lights[0];
    emitter.triangle = 1;
    const float corners[3][3] = {{-.25f, -.25f, -1.5f}, {.25f, -.25f, -1.5f}, {0, .25f, -1.5f}};
    for (uint32_t corner = 0; corner < 3; ++corner)
      std::copy_n(corners[corner], 3, emitter.corners[corner]);
    auto triangleJob = pipelines.prepare("triangle-visibility-test", [&] {
      transport.initialize(fixture.physical, fixture.device, triangleSource, options);
    });
    const auto triangleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!triangleJob->ready() && std::chrono::steady_clock::now() < triangleDeadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(triangleJob->ready(), "Triangle visibility transport preparation completed");
    triangleJob->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    transport.releaseBuildInputs();
    fixture.begin();
    transport.recordVisibility(fixture.command, 0, points, 1024);
    fixture.finish();
    transport.read(0, results);
    require(results[0].irradiance[0][0] < .05f && results[1].irradiance[0][0] > .25f &&
                results[1].irradiance[0][0] < .75f && results[2].irradiance[0][0] > .95f,
            "Triangle emitters retain weighted soft static visibility through "
            "the same material-aware ray path");
    std::printf("Triangle emitter visibility: umbra %.3f, penumbra %.3f, exposed %.3f\n",
                results[0].irradiance[0][0], results[1].irradiance[0][0], results[2].irradiance[0][0]);
    const auto farBlocker = world.wall(world.material({}), -200, 2);
    auto farSource = world.scene({receiver, farBlocker});
    options.queueFamily = VK_QUEUE_FAMILY_IGNORED;
    farSource->sun = source->sun;
    farSource->sun.angularDiameterDegrees = 0;
    auto farJob = pipelines.prepare("sun-complete-bounds-test", [&] {
      transport.initialize(fixture.physical, fixture.device, farSource, options);
    });
    const auto farDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!farJob->ready() && std::chrono::steady_clock::now() < farDeadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(farJob->ready(), "Distant sun caster preparation completed");
    farJob->check();
    require(!transport.timingSupported() && !transport.elapsed(0).has_value(),
            "A host without queue timestamp information keeps working without timing resources");
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    transport.releaseBuildInputs();
    fixture.begin();
    transport.recordSun(fixture.command, 0, sunPoints, 64);
    fixture.finish();
    transport.read(0, results);
    for (const auto &value : results)
      require(value.irradiance[0][0] == 0,
              "Static sun queries cover complete geometry beyond the indirect transport distance");
  }
  void siteValidation(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto material = world.material({});
    std::vector<uint64_t> faces;
    for (uint32_t axis = 0; axis < 3; ++axis)
      for (float sign : {-1.f, 1.f}) {
        std::array<Vec3, 4> corners{};
        constexpr float offsets[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (uint32_t c = 0; c < 4; ++c) {
          corners[c][axis] = sign;
          corners[c][(axis + 1) % 3] = offsets[c][0];
          corners[c][(axis + 2) % 3] = offsets[c][1];
        }
        Vec3 normal{};
        normal[axis] = sign;
        faces.push_back(world.quad(material, corners, nullptr, 0xffffffff, normal));
      }
    auto scene = world.scene({faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]});
    scene->dome.enabled = true;
    scene->dome.radiance = {1, 1, 1};
    bake::Transport transport;
    bake::Options options;
    options.batchPoints = 4;
    auto job = pipelines.prepare("capture-site-validation", [&] {
      transport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Capture site validation preparation completed");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    const bake::Point points[] = {{{0, 0, 0, 0}, {}}, {{2, 0, 0, 0}, {}}};
    fixture.begin();
    transport.recordSiteValidation(fixture.command, 0, points);
    fixture.finish();
    bake::Result results[2];
    transport.read(0, results);
    require(results[0].statistics[0] > .95f && results[1].statistics[0] < .05f &&
                results[0].statistics[2] == 64 && results[1].statistics[2] == 64,
            "Hardware candidate queries reject an origin inside a static prop while retaining free space");
    for (const auto &value : results)
      require(value.irradiance[0] == Vec4{}, "Site validation does not evaluate environment lighting");
    std::printf("Capture origin back-face fractions: inside %.3f, outside %.3f\n", results[0].statistics[0],
                results[1].statistics[0]);
  }
  void dielectricTransport(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    remixapi_MaterialInfoTranslucentEXT body{};
    body.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    body.refractiveIndex = 1.333f;
    body.transmittanceColor = {.5f, .25f, 1};
    body.transmittanceMeasurementDistance = 1;
    remixapi_MaterialBakeBoundaryEXT boundary{};
    boundary.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_BOUNDARY_EXT;
    boundary.pNext = &body;
    boundary.smoothBoundary = 1;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &boundary;
    info.hash = ++world.hash;
    remixapi_MaterialHandle material;
    world.resources.createMaterial(info, material);
    const auto top = world.quad(
        material, {{{-10000, -10000, 1}, {10000, -10000, 1}, {10000, 10000, 1}, {-10000, 10000, 1}}}, nullptr,
        0xffffffff, {0, 0, 1});
    const auto bottom = world.quad(
        material, {{{-10000, -10000, -1}, {10000, -10000, -1}, {10000, 10000, -1}, {-10000, 10000, -1}}},
        nullptr, 0xffffffff, {0, 0, -1});
    auto scene = world.scene({top, bottom});
    scene->dome.enabled = true;
    scene->dome.radiance = {1, 1, 1};
    bake::Transport transport;
    bake::Options options;
    options.batchPoints = 512;
    auto job = pipelines.prepare("dielectric-transport", [&] {
      transport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Dielectric preparation completed");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    Vec3 observed[3]{};
    for (uint32_t mode = 0; mode < 3; ++mode) {
      bake::Point ray;
      ray.position = {0, 0, mode == 1 ? 2.f : 0.f, 0};
      ray.normal = mode == 0 ? Vec4{0, 0, 1, 0} : mode == 1 ? Vec4{0, 0, -1, 0} : Vec4{1, 0, .1f, 0};
      std::vector<bake::Point> rays(512, ray);
      fixture.begin();
      transport.recordRadiance(fixture.command, 0, rays, 1);
      fixture.finish();
      std::vector<bake::Result> results(512);
      transport.read(0, results);
      for (const auto &value : results) {
        require(value.statistics[0] == 0, "A dielectric exit is not an invalid solid probe");
        for (uint32_t c = 0; c < 3; ++c) {
          require(std::isfinite(value.irradiance[0][c]), "Dielectric transport stays finite");
          observed[mode][c] += value.irradiance[0][c] / 512;
        }
      }
    }
    const auto &inside = observed[0], &outside = observed[1], &trapped = observed[2];
    require(inside[2] > 1.7f && inside[2] < 1.85f && inside[0] > inside[2] * .45f &&
                inside[0] < inside[2] * .52f && inside[1] > inside[2] * .21f && inside[1] < inside[2] * .27f,
            "Inside rays preserve Beer-Lambert color and radiance index scaling when leaving water");
    require(outside[2] > .99f && outside[2] < 1.01f && outside[0] > .20f && outside[0] < .34f &&
                outside[1] > .04f && outside[1] < .15f,
            "Two interfaces preserve clear-channel energy and absorb over the full interior path");
    require(trapped[0] == 0 && trapped[1] == 0 && trapped[2] == 0,
            "Total internal reflection cannot leak energy through a dielectric boundary");
    std::printf("Dielectric transport: inside %.3f %.3f %.3f; slab %.3f %.3f %.3f; TIR %.3f\n", inside[0],
                inside[1], inside[2], outside[0], outside[1], outside[2], trapped[2]);
  }
  void blendedCoverage(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaBlendEnabled = 1;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
    blend.writeMask = 15;
    float irradiance[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
      const auto wall = world.wall(world.material({.opacity = float(i) * .5f}), 1, 1000, &blend);
      auto scene = world.scene({wall});
      scene->dome.enabled = true;
      scene->dome.radiance = {1, 1, 1};
      const auto result = run(fixture, pipelines, world, scene, 1);
      irradiance[i] = evaluate(result, {0, 0, 1}, 0);
    }
    require(irradiance[0] > 2.5f && irradiance[2] < .3f && irradiance[1] > irradiance[0] * .35f &&
                irradiance[1] < irradiance[0] * .65f,
            "Source-over alpha admits proportional environment transport instead "
            "of becoming an opaque wall");
    const auto first = world.wall(world.material({.opacity = .5f}), 1, 1000, &blend);
    auto halfCovered = world.scene({first});
    halfCovered->dome.enabled = true;
    halfCovered->dome.radiance = {1, 1, 1};
    const float parallelHalf = evaluate(run(fixture, pipelines, world, halfCovered, 1, 8192), {0, 0, 1}, 0);
    const float serialHalf = evaluate(run(fixture, pipelines, world, halfCovered, 1, 8192, false), {0, 0, 1}, 0);
    require(std::abs(parallelHalf - std::numbers::pi_v<float> * .5f) < .08f &&
                std::abs(serialHalf - std::numbers::pi_v<float> * .5f) < .08f,
            "Parallel and serial high-sample coverage converge to the same analytic irradiance");
    std::printf("Coverage convergence at8192samples: parallel %.5f, serial %.5f, expected %.5f\n",
                  parallelHalf, serialHalf, std::numbers::pi_v<float> * .5f);
    const auto second = world.wall(world.material({.opacity = .5f}), 2, 1000, &blend);
    auto stacked = world.scene({first, second});
    stacked->dome.enabled = true;
    stacked->dome.radiance = {1, 1, 1};
    const float transmitted = evaluate(run(fixture, pipelines, world, stacked, 1), {0, 0, 1}, 0);
    require(transmitted > irradiance[0] * .12f && transmitted < irradiance[0] * .38f,
            "Stacked half-coverage sheets multiply transmittance rather than sharing one alpha decision");
    blend.textureAlphaOperation = 1;
    blend.textureAlphaArg1Source = 2; // terrain paint uses vertex alpha, not base texture alpha
    const auto paint = world.wall(world.material({}), 1, 1000, &blend, 0x80ffffff);
    auto painted = world.scene({paint});
    painted->dome.enabled = true;
    painted->dome.radiance = {1, 1, 1};
    const float vertexCoverage = evaluate(run(fixture, pipelines, world, painted, 1), {0, 0, 1}, 0);
    require(vertexCoverage > irradiance[0] * .35f && vertexCoverage < irradiance[0] * .65f,
            "Terrain sheet coverage follows its selected vertex-alpha operation");
    std::printf("Source-over irradiance: transparent %.3f, half coverage %.3f, "
                "opaque %.3f, stacked %.3f, vertex-alpha %.3f\n",
                irradiance[0], irradiance[1], irradiance[2], transmitted, vertexCoverage);
  }
  void bakeSurface(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto receiver = world.wall(world.material({}), 0, 2);
    const auto blocker =
        world.quad(world.material({}), {{{-4, -4, -.2f}, {0, -4, -.2f}, {0, 4, -.2f}, {-4, 4, -.2f}}});
    const uint64_t instances[] = {receiver, blocker};
    const float coordinates[] = {0, 0, 4, 4, 4, 0, 0, 0, 0, 4, 4, 4};
    const remixapi_BakeSurfaceChartEXT chart{receiver, 0, 0, 0, 6, 5, 5, coordinates};
    remixapi_BakeSceneSurfacesInfoEXT request{};
    auto &info = request.bounds.scene;
    info = {sizeof(request),
            REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT,
            1,
            ++world.revision,
            1,
            REMIXAPI_BAKE_SCENE_READY_EXT,
            2,
            0,
            instances,
            nullptr};
    for (uint32_t c = 0; c < 3; ++c) {
      request.bounds.minimum[c] = -8;
      request.bounds.maximum[c] = 8;
    }
    request.chartRevision = 1;
    request.chartCount = 1;
    request.charts = &chart;
    world.resources.setBakeScene(&info);
    auto scene = std::make_shared<scene::BakeScene>(*world.resources.bakeScene());
    scene->dome.enabled = true;
    scene->dome.radiance = {0, 1, 0};
    const auto layout = lightmap::plan(*scene, {16, 1, 2, 256});
    require(layout.points.size() == 25, "Authored surface chart produces actual hardware bake points");
    bake::Transport transport;
    bake::Options options;
    options.textureResolution = 32;
    options.batchPoints = 32;
    auto job = pipelines.prepare("surface-lightmap-test", [&] {
      transport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Surface bake preparation worker timed out");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    transport.releaseBuildInputs();
    fixture.begin();
    transport.recordBatch(fixture.command, 0, layout.points, 2048, 1);
    fixture.finish();
    std::vector<bake::Result> results(layout.points.size());
    transport.read(0, results);
    float left = -1, right = -1;
    for (uint32_t i = 0; i < layout.points.size(); ++i) {
      const auto &p = layout.points[i];
      const float green = evaluate(results[i], {p.normal[0], p.normal[1], p.normal[2]}, 1);
      if (std::abs(p.position[1]) < .01f) {
        if (p.position[0] < -1.9f)
          left = green;
        if (p.position[0] > 1.9f)
          right = green;
      }
      require(results[i].statistics[2] == 2048,
              "Hardware transport returns a separate result for every surface texel");
    }
    require(left >= 0 && right > 1.5f && left < right * .5f,
            "Surface texels resolve static occlusion independently of irradiance "
            "probe spacing");
    for (auto owner : layout.owners)
      if (owner != lightmap::unowned)
        require(owner < results.size(), "Atlas padding addresses completed surface results");
    std::printf("Surface texel readback: occluded %.3f, exposed %.3f green irradiance\n", left, right);
  }
  void captureProbe(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto emitter = world.wall(world.material({.diffuse = .5f, .emission = 3}), 1, 2);
    auto scene = world.scene({emitter});
    scene->dome.enabled = true;
    scene->dome.radiance = {0, 0, 1};
    probes::Capture capture;
    capture.bounds.include({-1, -1, -1});
    capture.bounds.include({1, 1, 1});
    capture.resolution = 8;
    const auto rays = probes::rays(capture);
    bake::Transport transport;
    bake::Options options;
    options.batchPoints = uint32_t(rays.size());
    options.textureResolution = 32;
    auto job = pipelines.prepare("bake-cube-test-" + std::to_string(world.revision), [&] {
      transport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Cubemap capture preparation completed");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    transport.recordRadiance(fixture.command, 0, rays, 1);
    fixture.finish();
    std::vector<bake::Result> results(rays.size());
    transport.read(0, results);
    transport.releaseBuildInputs();
    for (const auto &result : results)
      capture.radiance.push_back(result.irradiance[0]);
    const auto &positiveZ = capture.radiance[4 * 64 + 36];
    const auto &negativeZ = capture.radiance[5 * 64 + 36];
    require(std::abs(positiveZ[0] - 3) < 1e-5f && positiveZ[1] == 0 && positiveZ[2] == 0,
            "Directional capture sees the emissive wall without irradiance "
            "convolution");
    require(negativeZ[0] == 0 && negativeZ[1] == 0 && std::abs(negativeZ[2] - 1) < 1e-5f,
            "Opposite cube face captures the environment behind the probe");
    require(positiveZ[3] > 1 && positiveZ[3] < 1.1f,
            "Directional radiance retains first-hit distance for capture "
            "diagnostics");
    bool refused = false;
    const bake::Point invalid{};
    try {
      transport.recordRadiance(fixture.command, 0, {&invalid, 1});
    } catch (const std::invalid_argument &) {
      refused = true;
    }
    require(refused, "A zero reflection capture direction is refused before dispatch");
    const auto data = probes::filter({&capture, 1}, 256);
    test::probeQueries(fixture, data);
    capture.influence.include({-2, -2, -2});
    capture.influence.include({2, 2, 2});
    const auto extended = probes::filter({&capture, 1}, 64);
    for (uint32_t skyCase = 1; skyCase <= 3; ++skyCase)
      test::probeQueries(fixture, extended, skyCase);
    LightData lamp;
    lamp.positionRadius = {0, 0, 0, .4f};
    lamp.radiance = {5, 5, 5, 0};
    lamp.directionCone[3] = -1;
    transport.updateLighting(0, {&lamp, 1}, scene->sun, scene->dome, 100);
    fixture.begin();
    transport.recordRadiance(fixture.command, 0, rays, 2);
    fixture.finish();
    std::vector<bake::Result> reference(rays.size());
    transport.read(0, reference);
    // Cubemap faces and live reflection rays may be chunked differently. The
    // same origin/direction must retain its random stream when its batch index
    // changes; this fixture includes stochastic finite-emitter sampling.
    for (size_t first = 0; first < rays.size(); first += 37) {
      const size_t count = std::min(size_t(37), rays.size() - first);
      fixture.begin();
      transport.recordRadiance(fixture.command, 0, std::span(rays).subspan(first, count), 2);
      fixture.finish();
      std::vector<bake::Result> chunk(count);
      transport.read(0, chunk);
      for (size_t i = 0; i < count; ++i)
        for (uint32_t c = 0; c < 4; ++c)
          require(std::abs(chunk[i].irradiance[0][c] - reference[first + i].irradiance[0][c]) < 1e-5f,
                  "Radiance sampling is independent of batch boundaries");
    }
    std::printf("Cubemap readback: +Z red %.3f at %.3f units, -Z blue %.3f; "
                "CPU/GPU box projection agrees\n",
                positiveZ[0], positiveZ[3], negativeZ[2]);
  }
} // namespace
int main() try {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {
    test::Fixture fixture;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(fixture.physical, &properties);
    pipeline::Service pipelines(fixture.device, properties, false);
    World world(fixture);
    auto environment = world.scene({});
    environment->dome.enabled = true;
    environment->dome.radiance = {.2f, .4f, .8f};
    auto sky = run(fixture, pipelines, world, environment);
    for (Vec3 normal : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}})
      for (uint32_t c = 0; c < 3; ++c)
        require(std::abs(evaluate(sky, normal, c) -
                         std::numbers::pi_v<float> * environment->dome.radiance[c]) < .04f,
                "Constant environment integrates to pi times radiance in every "
                "direction");
    require(sky.statistics[1] == 1, "Empty-world environment rays miss geometry");
    auto hdr = std::make_shared<scene::Texture>();
    hdr->pixels.width = 2;
    hdr->pixels.height = 1;
    hdr->pixels.srgb = false;
    hdr->pixels.mips = {{0, 0, 0, 255, 0, 0, 0, 255}};
    hdr->pixels.hdrMips = {{4, 0, 0, 1, 0, 2, 0, 1}};
    auto directional = world.scene({});
    directional->dome.enabled = true;
    directional->dome.radiance = {1, 1, 1};
    directional->dome.texture = hdr;
    const auto hdrSky = run(fixture, pipelines, world, directional);
    require(evaluate(hdrSky, {0, 0, -1}, 0) > 10 && evaluate(hdrSky, {0, 0, 1}, 1) > 5 &&
                evaluate(hdrSky, {0, 0, 1}, 0) < 1,
            "HDR environment atlas preserves directional radiance above one");
    directional->dome.worldToLocal[0] = directional->dome.worldToLocal[10] = -1;
    ++world.revision;
    const auto rotatedSky = run(fixture, pipelines, world, directional);
    require(evaluate(rotatedSky, {0, 0, 1}, 0) > 10 && evaluate(rotatedSky, {0, 0, -1}, 1) > 5,
            "Environment world-to-local rotation changes baked irradiance "
            "directions");
    for (const auto &face : sky.moments)
      require(std::abs(face[0] - 100) < .01f && std::abs(face[1] - 10000) < 1 && face[3] > 0,
              "Directional distance moments retain unoccluded environment reach");
    const auto emitter = world.wall(world.material({.emission = 2}), 1, 2);
    auto emitted = run(fixture, pipelines, world, world.scene({emitter}));
    const float red = emitted.irradiance[0][0];
    require(red > 1 && emitted.irradiance[0][1] < 1e-5f,
            "Emissive geometry supplies coloured baked radiance");
    const auto decorative = world.wall(world.material({.emission = 2, .suppress = true}), 1, 2);
    const auto unlit = run(fixture, pipelines, world, world.scene({decorative}));
    require(std::abs(unlit.irradiance[0][0] - red) < .01f,
            "Suppression of generated lights preserves radiance on emissive "
            "ray hits");
    const auto self = world.wall(world.material({.diffuse = .5f, .emission = 2, .self = true}), 1, 2);
    auto selfScene = world.scene({self});
    const auto selfEmitted = run(fixture, pipelines, world, selfScene, 1);
    require(std::abs(selfEmitted.irradiance[0][0] - red * .5f) < .02f,
            "Self illumination multiplies colour and emission strength instead "
            "of adding a second lobe");
    selfScene->dome.enabled = true;
    selfScene->dome.radiance = {1, 1, 1};
    ++world.revision;
    const auto selfSingle = run(fixture, pipelines, world, selfScene, 1);
    ++world.revision;
    const auto selfBounce = run(fixture, pipelines, world, selfScene, 2);
    require(std::abs(selfSingle.irradiance[0][0] - selfBounce.irradiance[0][0]) < .01f,
            "A fully self-illuminated surface does not also reflect diffuse "
            "environment light");
    const uint8_t maskPixels[] = {0, 128, 0, 255, 0, 128, 0, 255};
    world.texture(0x103, maskPixels, REMIXAPI_FORMAT_R8G8B8A8_SRGB);
    const auto masked =
        world.wall(world.material({.diffuse = .5f, .emission = 2, .emissive = L"0x103", .self = true}), 1, 2);
    const auto selfMasked = run(fixture, pipelines, world, world.scene({masked}), 1);
    require(std::abs(selfMasked.irradiance[0][1] / red - 128.f / 510) < .01f,
            "Self illumination samples an sRGB source's mask as raw data");
    const auto blocker = world.wall(world.material({}), .5f, 2);
    auto blocked = run(fixture, pipelines, world, world.scene({emitter, blocker}));
    require(blocked.irradiance[0][0] < red * .01f, "Opaque geometry occludes emissive transport");
    const uint8_t cutoutPixels[] = {255, 255, 255, 0, 255, 255, 255, 255};
    world.texture(0x100, cutoutPixels);
    const auto fence = world.wall(world.material({.albedo = L"0x100", .cutout = true}), .5f, 2);
    auto holes = run(fixture, pipelines, world, world.scene({emitter, fence}));
    require(holes.irradiance[0][0] > red * .2f && holes.irradiance[0][0] < red * .8f,
            "Textured alpha coverage transmits through holes and blocks the "
            "solid half");
    const uint8_t greenPixels[] = {0, 255, 0, 255, 0, 255, 0, 255};
    world.texture(0x101, greenPixels);
    const auto green = world.wall(world.material({.emission = 2, .emissive = L"0x101"}), 1, 2);
    auto mapped = run(fixture, pipelines, world, world.scene({green}));
    require(mapped.irradiance[0][1] > 1 && mapped.irradiance[0][0] < 1e-5f,
            "Emission texture atlas preserves radiance colour");
    const auto diffuse = world.wall(world.material({.diffuse = .8f}), 1, 2);
    auto bounceScene = world.scene({diffuse});
    bounceScene->dome.enabled = true;
    bounceScene->dome.radiance = {1, 1, 1};
    const auto single = run(fixture, pipelines, world, bounceScene, 1);
    ++world.revision;
    const auto bounced = run(fixture, pipelines, world, bounceScene, 2);
    require(bounced.irradiance[0][0] > single.irradiance[0][0] + .5f,
            "Diffuse continuation transports an additional environment bounce");
    const auto mirror = world.wall(world.material({.diffuse = .8f, .metallic = 1}), 1, 2);
    auto metalScene = world.scene({mirror});
    metalScene->dome.enabled = true;
    metalScene->dome.radiance = {1, 1, 1};
    const auto metal = run(fixture, pipelines, world, metalScene, 2);
    require(std::abs(metal.irradiance[0][0] - single.irradiance[0][0]) < .02f,
            "Metallic surfaces do not create a diffuse bounce");
    auto sunScene = world.scene({diffuse});
    sunScene->sun.isDistant = true;
    sunScene->sun.direction[2] = 1;
    sunScene->sun.direction[1] = 0;
    sunScene->sun.radiance[0] = sunScene->sun.radiance[1] = sunScene->sun.radiance[2] = 1;
    const auto sunlight = run(fixture, pipelines, world, sunScene, 1);
    require(sunlight.irradiance[0][0] > .1f, "Sunlit geometry supplies indirect radiance");
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {2, 0, 0};
    sphere.radius = .25f;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &sphere;
    light.hash = 0x200;
    light.radiance = {50, 50, 50};
    remixapi_LightHandle lamp;
    world.resources.createLight(light, lamp);
    const auto patch = world.wall(world.material({.diffuse = .8f}), 1, .5f);
    const auto lit = run(fixture, pipelines, world, world.scene({patch}, {lamp}), 1);
    const auto shade = world.quad(world.material({}), {{{1, -3, -.5f}, {1, 3, -.5f}, {1, 3, 2}, {1, -3, 2}}});
    const auto shadowed = run(fixture, pipelines, world, world.scene({patch, shade}, {lamp}), 1);
    require(lit.irradiance[0][0] > .01f && shadowed.irradiance[0][0] < lit.irradiance[0][0] * .05f,
            "Hardware visibility shadows local-light indirect radiance without "
            "hiding the receiver patch");
    captureProbe(fixture, pipelines, world);
    siteValidation(fixture, pipelines, world);
    dielectricTransport(fixture, pipelines, world);
    blendedCoverage(fixture, pipelines, world);
    bakeSurface(fixture, pipelines, world);
    bakeVisibility(fixture, pipelines, world);
    liveLighting(fixture, pipelines, world);
    std::printf("Bake readback: emitter %.4f, blocked %.4f, alpha holes %.4f, "
                "local light %.4f, shadowed %.4f\n",
                red, blocked.irradiance[0][0], holes.irradiance[0][0], lit.irradiance[0][0],
                shadowed.irradiance[0][0]);
  }
  require(test::validationErrors == 0, "Bake transport emitted Vulkan validation errors");
  std::puts("Hardware diffuse bake: environment SH, emission, textured "
            "alpha/emission, sun/local visibility, "
            "metallic rejection and multiple bounces passed");
} catch (VkResult result) {
  std::fprintf(stderr, "Bake test Vulkan failure %d\n", int(result));
  return 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "Bake test failed: %s\n", error.what());
  return 1;
}
