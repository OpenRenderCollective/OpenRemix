#include "forward/bake/transport.h"
#include "forward/bake/budget.h"
#include "forward/bake/probe_visibility.h"
#include "forward/bake/probe_visibility_plan.h"
#include "forward/lightmap/layout.h"
#include "forward/mixed/overflow.h"
#include "gpu_fixture.h"
#include "probe_queries.h"
#include "lighting_components.h"
#include "raster/renderer.h"
#include "runtime/pipelines.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>
#include <thread>

using namespace remix;
using namespace remix::forward;
using remix::forward::bake::TransportLight;
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
                  Vec3 normal = {}, bool decal = false) {
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
      instance.categoryFlags = decal ? REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC : 0;
      instance.doubleSided = 1;
      instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
      return resources.createRetained(instance);
    }
    uint64_t wall(remixapi_MaterialHandle material, float z, float size,
                  remixapi_InstanceInfoBlendEXT *blend = nullptr, uint32_t color = 0xffffffff,
                  bool decal = false) {
      return quad(material, {{{-size, -size, z}, {size, -size, z}, {size, size, z}, {-size, size, z}}}, blend,
                  color, {}, decal);
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
#include "indexed_visibility.h"
#include "probe_visibility_gpu.h"
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
  void lightSelectionVariance(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto wall = world.wall(world.material({.diffuse = 1}), 1, 2);
    auto source = world.scene({wall});
    source->sun.isDistant = false;
    source->dome.enabled = false;
    source->lights.clear();
    for (uint32_t i = 0; i < 128; ++i) {
      scene::SphereLight light;
      light.id = i + 1;
      light.position[2] = -1;
      light.radius = .1f;
      light.radiance[0] = light.radiance[1] = light.radiance[2] = i ? .0001f : 1;
      source->lights.push_back(light);
    }
    const bake::Point ray{{0, 0, 0, 0}, {0, 0, 1, 0}};
    const double distance = 2 - .002; // receiver bias points toward the lights
    const double expected = (1 + 127 * .0001) * .1 * .1 / (distance * distance);
    double mse[2]{}, mean[2]{};
    for (uint32_t weighted = 0; weighted < 2; ++weighted) {
      bake::Transport transport;
      bake::Options options;
      options.batchPoints = 1;
      options.textureResolution = 16;
      options.lightImportanceSampling = weighted != 0;
      auto ticket = pipelines.prepare("bake-selection-" + std::to_string(weighted), [&] {
        transport.initialize(fixture.physical, fixture.device, source, options);
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!ticket->ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(ticket->ready(), "Light selection fixture prepares");
      ticket->check();
      fixture.begin();
      transport.recordPrepare(fixture.command, world.resources);
      fixture.finish();
      for (uint32_t batch = 0; batch < 64; ++batch) {
        fixture.begin();
        transport.recordRadiance(fixture.command, 0, {&ray, 1}, 1, 64, batch * 64);
        fixture.finish();
        bake::Result result;
        transport.read(0, {&result, 1});
        const double value = result.irradiance[0][0];
        require(std::isfinite(value), "Light selection returns finite energy");
        mean[weighted] += value / 64;
        mse[weighted] += std::pow(value - expected, 2) / 64;
      }
    }
    require(std::abs(mean[1] - expected) < expected * .05,
            "Spatial light selection preserves the independent analytic lighting sum");
    require(mse[1] < mse[0] * .1,
            "Importance selection reduces error without increasing the ray/sample count");
    std::printf("128-light selection: uniform RMSE %.7f, importance %.7f; weighted mean %.7f, "
                "analytic %.7f\n", std::sqrt(mse[0]), std::sqrt(mse[1]), mean[1], expected);
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
    TransportLight light;
    light.positionRadius = {0, 0, -1, .25f};
    light.radiance = {100, 0, 0, 0};
    light.directionCone = {0, 0, -1, -1};
    const bake::Point ray{{0, 0, 0, 0}, {0, 0, 1, 0}};
    auto sample = [&](uint32_t slot, std::span<const TransportLight> lights) {
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
    std::vector<TransportLight> many(65, light);
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
  void aggregateDiffuse(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto black = world.material({});
    const auto background = world.quad(black, {{{-4, -4, -3}, {4, -4, -3}, {4, 4, -3}, {-4, 4, -3}}});
    auto source = world.scene({background});
    scene::SphereLight red;
    red.id = 91;
    red.radius = .25f;
    red.position[2] = 2;
    red.radiance[0] = 3;
    scene::SphereLight blue = red;
    blue.id = 19;
    blue.position[0] = .5f;
    blue.radiance[0] = 0;
    blue.radiance[2] = 5;
    scene::SphereLight green = red;
    green.id = 51;
    green.triangle = 1;
    green.radiance[0] = 0;
    green.radiance[1] = 2;
    const float corners[3][3] = {{-.3f, -.25f, 2}, {0, .3f, 2}, {.3f, -.25f, 2}};
    for (uint32_t corner = 0; corner < 3; ++corner)
      std::copy_n(corners[corner], 3, green.corners[corner]);
    source->lights = {red, blue, green};
    source->dome.enabled = true;
    source->dome.radiance = {.1f, .1f, .1f};
    const uint32_t baked[] = {0, 2};
    bake::Options options;
    options.textureResolution = 32;
    options.batchPoints = 8;
    options.rayBudget = 1u << 18;
    options.directLighting = bake::directPolicy(*source, baked);
    const auto initialize = [&](bake::Transport &transport, const auto &input, const auto &settings,
                                 const char *name) {
      auto ticket = pipelines.prepare(name, [&] {
        transport.initialize(fixture.physical, fixture.device, input, settings);
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!ticket->ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(ticket->ready(), "Aggregate diffuse transport preparation completed");
      ticket->check();
      fixture.begin();
      transport.recordPrepare(fixture.command, world.resources);
      fixture.finish();
      transport.releaseBuildInputs();
    };
    bake::Transport aggregate, legacy;
    initialize(aggregate, source, options, "aggregate-diffuse-fixture");
    auto legacyOptions = options;
    legacyOptions.directLighting = {};
    initialize(legacy, source, legacyOptions, "aggregate-diffuse-reference");
    const bake::Point point{};
    fixture.begin();
    aggregate.recordBatch(fixture.command, 0, {&point, 1}, 8192, 1);
    legacy.recordBatch(fixture.command, 0, {&point, 1}, 8192, 1);
    fixture.finish();
    bake::Result actual, indirect;
    aggregate.read(0, {&actual, 1});
    legacy.read(0, {&indirect, 1});
    bake::VisibilityPoint connections[] = {{point, 0}, {point, 1}, {point, 2}};
    fixture.begin();
    aggregate.recordVisibility(fixture.command, 1, connections, 8192, true);
    fixture.finish();
    bake::VisibilityResult direct[3];
    aggregate.readVisibility(1, direct);
    bake::Result reference;
    bake::Result contribution = actual;
    for (uint32_t coefficient = 0; coefficient < 9; ++coefficient) {
      reference.irradiance[coefficient][0] = direct[0].irradiance[coefficient] * 3;
      reference.irradiance[coefficient][1] = direct[2].irradiance[coefficient] * 2;
      require(actual.irradiance[coefficient][2] == indirect.irradiance[coefficient][2],
              "Mixed blue adds no direct energy and the existing environment contribution is not doubled");
      for (uint32_t color = 0; color < 3; ++color)
        contribution.irradiance[coefficient][color] -= indirect.irradiance[coefficient][color];
    }
    require(evaluate(indirect, {0, 0, 1}, 2) > .1f,
            "The additive energy reference contains nonzero pre-existing irradiance");
    for (const Vec3 normal : {Vec3{0, 0, 1}, Vec3{.6f, 0, .8f}, Vec3{0, -.6f, .8f}})
      for (uint32_t color = 0; color < 2; ++color) {
        const float expected = evaluate(reference, normal, color), measured = evaluate(contribution, normal, color);
        if (!(expected > 0 && std::abs(measured - expected) < .04f * expected))
          std::fprintf(stderr, "Aggregate mismatch: normal %.3f %.3f %.3f, color %u, measured %.9g expected %.9g\n",
                        normal[0], normal[1], normal[2], color, measured, expected);
        require(expected > 0 && std::abs(measured - expected) < .04f * expected,
                "Aggregate sphere/triangle irradiance agrees with independently attributed direct SH");
      }
    const bake::Point ray{{0, 0, 0, 0}, {0, 0, 1, 0}};
    fixture.begin();
    aggregate.recordRadiance(fixture.command, 0, {&ray, 1}, 1);
    fixture.finish();
    bake::Result capture;
    aggregate.read(0, {&capture, 1});
    require(capture.irradiance[0] == Vec4{.1f, .1f, .1f, options.distance},
            "Radiance captures retain outgoing-radiance semantics without aggregate irradiance added");
    auto serialSource = std::make_shared<scene::BakeScene>(*source);
    serialSource->dome.enabled = false;
    auto serialOptions = options;
    serialOptions.parallel = false;
    bake::Transport serial;
    initialize(serial, serialSource, serialOptions, "aggregate-diffuse-serial");
    fixture.begin();
    serial.recordBatch(fixture.command, 0, {&point, 1}, 8192, 1);
    fixture.finish();
    bake::Result serialResult;
    serial.read(0, {&serialResult, 1});
    for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
      for (uint32_t color = 0; color < 3; ++color)
        require(std::abs(serialResult.irradiance[coefficient][color] -
                          contribution.irradiance[coefficient][color]) < 2e-5f,
                "Serial and parallel aggregate kernels retain the same ordinal-based direct samples");
    const auto blocker = world.quad(black, {{{-4, -4, 1}, {4, -4, 1}, {4, 4, 1}, {-4, 4, 1}}});
    auto occluded = world.scene({background, blocker});
    occluded->lights = source->lights;
    bake::Transport blocked;
    initialize(blocked, occluded, options, "aggregate-diffuse-blocked");
    fixture.begin();
    blocked.recordBatch(fixture.command, 0, {&point, 1}, 8192, 1);
    fixture.finish();
    bake::Result shadow;
    blocked.read(0, {&shadow, 1});
    for (const auto &coefficient : shadow.irradiance)
      require(coefficient == Vec4{}, "Fully baked direct cannot pass through an opaque wall");
    std::printf("Aggregate direct: red %.6f/%.6f, triangle green %.6f/%.6f; mixed blue 0, behind wall 0\n",
                 evaluate(contribution, {0, 0, 1}, 0), evaluate(reference, {0, 0, 1}, 0),
                 evaluate(contribution, {0, 0, 1}, 1), evaluate(reference, {0, 0, 1}, 1));
  }
  void overflowVisibility(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto receiver = world.wall(world.material({}), 0, 2);
    const auto blocker = world.quad(world.material({}),
        {{{-3, -3, -.5f}, {0, -3, -.5f}, {0, 3, -.5f}, {-3, 3, -.5f}}});
    auto source = world.scene({receiver, blocker});
    source->charts.push_back({0, 0, 2, 2, {{0, 0}, {1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, 1}}});
    for (uint32_t light = 0; light < 5; ++light) {
      scene::SphereLight lamp;
      lamp.id = light + 1;
      lamp.radius = .25f;
      lamp.position[0] = light == 4 ? -1.f : 0.f;
      lamp.position[2] = -1.5f;
      lamp.radiance[0] = lamp.radiance[1] = lamp.radiance[2] = 1;
      source->lights.push_back(lamp);
    }
    lightmap::Layout layout;
    layout.charts.resize(1);
    layout.points.push_back({{0, 0, 0, 0}, {0, 0, -1, 0}});
    layout.pointCharts.push_back(0);
    auto plan = mixed::plan(source, layout);
    require(plan.overflow == 1 && plan.charts[0] == bake::UInt4{0, 1, 2, 3},
            "Four equally important penumbra lights occupy the finite visibility channels");
    bake::Transport transport;
    bake::Options options;
    options.textureResolution = 32;
    options.batchPoints = 8;
    options.queueFamily = fixture.family;
    auto job = pipelines.prepare("overflow-visibility-test", [&] {
      transport.initialize(fixture.physical, fixture.device, source, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Overflow transport preparation completed");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    transport.releaseBuildInputs();
    bake::VisibilityPoint points[5];
    for (uint32_t light = 0; light < 5; ++light)
      points[light] = {layout.points[0], light};
    fixture.begin();
    transport.recordVisibility(fixture.command, 0, points, 1024);
    fixture.finish();
    bake::VisibilityResult results[5];
    transport.readVisibility(0, results);
    Vec4 partial{};
    for (uint32_t light = 0; light < 4; ++light) {
      partial[light] = results[light].visibility;
      require(partial[light] > .25f && partial[light] < .75f,
              "Four real finite emitters have partial hardware visibility");
    }
    require(results[4].visibility == 0, "The overflow emitter is wholly behind the static blocker");
    require(!mixed::refine(plan, layout, {&partial, 1}) && plan.overflow == 1 &&
                plan.occlusion.state(0, 4) == mixed::Visibility::Unknown,
            "Old channel traversal leaves a physically blocked fifth emitter unclassified");
    const auto owners = plan.charts;
    mixed::OverflowAudit audit;
    require(audit.nextRound(plan, layout) && audit.tasks().size() == 1 && audit.tasks()[0].light == 4,
            "The overflow audit schedules the previously skipped emitter");
    fixture.begin();
    const bake::VisibilityPoint extra{layout.points[0], audit.tasks()[0].light};
    transport.recordVisibility(fixture.command, 1, {&extra, 1}, 1024);
    fixture.finish();
    bake::VisibilityResult hidden;
    transport.readVisibility(1, {&hidden, 1});
    audit.observe(0, hidden.visibility, layout);
    require(!audit.nextRound(plan, layout) && plan.overflow == 0 && plan.charts == owners &&
                plan.occlusion.state(0, 4) == mixed::Visibility::Blocked &&
                plan.lights.match(source->lights[4]) == 4,
            "Hardware-verified hidden overflow receives a runtime mask without losing penumbra channels");
    std::printf("Overflow fixture: four penumbrae %.3f %.3f %.3f %.3f, hidden %.3f, classified blocked\n",
                partial[0], partial[1], partial[2], partial[3], hidden.visibility);
  }
  void bakeVisibility(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto receiver = world.wall(world.material({}), 0, 2);
    // Alpha is irrelevant to this opaque material, including zero-alpha texels.
    world.texture(0x5cab, {255, 255, 255, 0, 255, 255, 255, 255});
#if defined(_WIN32)
    const auto opaqueTexture = L"0x5cab";
#else
    const auto opaqueTexture = "0x5cab";
#endif
    const auto blocker =
        world.quad(world.material({.albedo = opaqueTexture}),
                    {{{-3, -3, -.5f}, {0, -3, -.5f}, {0, 3, -.5f}, {-3, 3, -.5f}}});
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
    options.batchPoints = 128;
    options.initialBatchPoints = 2;
    options.queueFamily = fixture.family;
    if (std::getenv("REMIX_BENCH_VISIBILITY")) {
      options.batchPoints = 12288;
      options.rayBudget = 16u << 20;
    }
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
    indexedVisibilityTests(fixture, pipelines, world, source);
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
    if (const char *output = std::getenv("REMIX_BENCH_VISIBILITY")) {
      std::vector<bake::VisibilityPoint> batch(12288);
      for (size_t i = 0; i < batch.size(); ++i) batch[i] = points[i % 3];
      std::vector<bake::VisibilityResult> measured(batch.size());
      for (uint32_t repeat = 0; repeat < 9; ++repeat) {
        fixture.begin();
        transport.recordVisibility(fixture.command, 0, batch, 256, false);
        fixture.finish();
        transport.readVisibility(0, measured);
        std::printf("Visibility benchmark %u: %.6f ms\n", repeat, transport.elapsed(0).value_or(0));
      }
      std::ofstream capture(output, std::ios::binary);
      capture.write(reinterpret_cast<const char *>(measured.data()),
                     std::streamsize(measured.size() * sizeof(measured[0])));
    }
    // Compare the compact wire format against the original full-result GPU
    // kernel, including partial workgroups and both local/sun query modes.
    bake::Transport legacy;
    auto legacyOptions = options;
    legacyOptions.cpuReadback = false;
    auto legacyJob = pipelines.prepare("visibility-full-result-reference", [&] {
      legacy.initialize(fixture.physical, fixture.device, source, legacyOptions);
    });
    const auto legacyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!legacyJob->ready() && std::chrono::steady_clock::now() < legacyDeadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(legacyJob->ready(), "Full-result visibility reference preparation completed");
    legacyJob->check();
    fixture.begin();
    legacy.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    const bake::Point referenceSunPoints[] = {points[0].surface, points[1].surface, points[2].surface};
    for (bool sun : {false, true})
      for (bool directDiffuse : {false, true})
        for (uint32_t samples : {1u, 63u, 65u, 1024u}) {
          fixture.begin();
          if (sun) {
            transport.recordSun(fixture.command, 0, referenceSunPoints, samples, directDiffuse);
            legacy.recordSun(fixture.command, 0, referenceSunPoints, samples, directDiffuse);
          } else {
            transport.recordVisibility(fixture.command, 0, points, samples, directDiffuse);
            legacy.recordVisibility(fixture.command, 0, points, samples, directDiffuse);
          }
          fixture.finish();
          std::array<bake::VisibilityResult, 3> compact;
          std::array<bake::Result, 3> full;
          transport.readVisibility(0, compact);
          if (!sun && !directDiffuse) {
            std::array<float, 3> scalar;
            transport.readScalarVisibility(0, scalar);
            for (uint32_t i = 0; i < scalar.size(); ++i)
              require(scalar[i] == compact[i].visibility,
                      "Scalar overflow readback preserves each visibility without directional expansion");
          }
          legacy.read(0, full);
          transport.read(0, results);
          require(std::memcmp(full.data(), results, sizeof(results)) == 0,
                  "Compact visibility expands byte-identically to the original GPU result contract");
          for (uint32_t i = 0; i < 3; ++i) {
            require(compact[i].visibility == full[i].irradiance[0][0],
                    "Compact local/sun visibility preserves exact blocked/clear endpoints");
            for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
              require(compact[i].irradiance[coefficient] == full[i].irradiance[coefficient][1],
                      "Compact unit direct SH preserves every coefficient without quantization");
          }
        }
    std::puts("Compact/full visibility: local/sun, scalar/direct SH and partial workgroups are byte-identical");
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
    {
      std::array<bake::Point, 12> duplicateSites;
      for (uint32_t receiver = 0; receiver < 3; ++receiver)
        std::fill_n(duplicateSites.begin() + receiver * 4, 4, sunPoints[receiver]);
      fixture.begin();
      transport.recordSurfaceSun(fixture.command, 0, duplicateSites, 1024, true);
      fixture.finish();
      bake::Result duplicateResults[3];
      transport.read(0, duplicateResults);
      require(std::memcmp(results, duplicateResults, sizeof(results)) == 0,
              "Duplicated receiver sites preserve the original sun sample stream and result exactly");
      const bake::Point crossing[] = {sunPoints[0], sunPoints[2], sunPoints[0], sunPoints[2]};
      fixture.begin();
      transport.recordSurfaceSun(fixture.command, 1, crossing, 1024, true);
      fixture.finish();
      bake::Result integrated;
      transport.read(1, {&integrated, 1});
      require(integrated.irradiance[0][0] == .5f && integrated.statistics[2] == 1024,
              "A receiver footprint crossing a hard shadow integrates half coverage with the original ray count");
      const float integratedDirect = evaluate(integrated, {0, 0, -1}, 1);
      require(integratedDirect > .4f && integratedDirect < .65f,
              "Receiver-area direct SH and visibility use the same blocked and exposed surface sites");
      bake::VisibilityPoint localSites[4];
      for (uint32_t site = 0; site < 4; ++site) localSites[site] = {crossing[site], 0};
      fixture.begin();
      transport.recordSurfaceVisibility(fixture.command, 0, localSites, 1024, true);
      fixture.finish();
      transport.read(0, {&integrated, 1});
      require(integrated.irradiance[0][0] > .45f && integrated.irradiance[0][0] < .55f &&
                  integrated.statistics[2] == 1024,
              "Local emitter visibility integrates receiver area without multiplying the sample budget");
      bool partialRejected = false, unbalancedRejected = false;
      try {
        transport.recordSurfaceSun(fixture.command, 0, std::span(crossing).first(3), 1024);
      } catch (const std::invalid_argument &) { partialRejected = true; }
      try {
        transport.recordSurfaceSun(fixture.command, 0, crossing, 6);
      } catch (const std::invalid_argument &) { unbalancedRejected = true; }
      require(partialRejected && unbalancedRejected,
              "Receiver-area batches reject incomplete sites and unequal sample allocation before recording");
      std::array<bake::Point, 48> batchSites;
      std::array<bake::VisibilityPoint, 48> batchLocal;
      for (uint32_t receiver = 0; receiver < 12; ++receiver)
        for (uint32_t site = 0; site < 4; ++site) {
          auto point = sunPoints[1];
          point.position[0] = .03f * (float(receiver) - 6) + .12f * (float(site) - 1.5f);
          batchSites[receiver * 4 + site] = point;
          batchLocal[receiver * 4 + site] = {point, 0};
        }
      for (bool sunlight : {false, true}) {
        auto recordSites = [&](uint32_t slot, size_t first, size_t count) {
          if (sunlight)
            transport.recordSurfaceSun(fixture.command, slot, std::span(batchSites).subspan(first * 4, count * 4), 64, true);
          else
            transport.recordSurfaceVisibility(fixture.command, slot, std::span(batchLocal).subspan(first * 4, count * 4), 64, true);
        };
        fixture.begin();
        recordSites(0, 0, 12);
        fixture.finish();
        std::array<bake::VisibilityResult, 12> reference;
        transport.readVisibility(0, reference);
        for (size_t batch : {1u, 3u, 7u})
          for (size_t first = 0; first < reference.size(); first += batch) {
            const auto count = std::min(batch, reference.size() - first);
            const auto slot = uint32_t(first % 2);
            fixture.begin();
            recordSites(slot, first, count);
            fixture.finish();
            std::vector<bake::VisibilityResult> measured(count);
            transport.readVisibility(slot, measured);
            require(std::memcmp(measured.data(), reference.data() + first, count * sizeof(measured[0])) == 0,
                    "Receiver-area sun/local masks and SH preserve exact sample identity across chunks and frame slots");
          }
      }
      std::printf("Receiver-area integration: sun visibility 0.5, direct %.5f, local %.5f, rays 1024\n",
                  integratedDirect, integrated.irradiance[0][0]);
    }
    std::vector<bake::VisibilityPoint> receivers(127);
    std::vector<bake::Point> sunlight(127);
    for (uint32_t i = 0; i < receivers.size(); ++i) {
      auto point = points[1];
      point.surface.position[0] = -.8f + 1.6f * float(i % 17) / 16;
      point.surface.position[1] = -.6f + 1.2f * float(i / 17) / 7;
      receivers[i] = point;
      sunlight[i] = point.surface;
    }
    for (bool sun : {false, true}) {
      auto record = [&](uint32_t slot, size_t first, size_t count) {
        if (sun)
          transport.recordSun(fixture.command, slot, std::span(sunlight).subspan(first, count), 256, true);
        else
          transport.recordVisibility(fixture.command, slot, std::span(receivers).subspan(first, count), 256, true);
      };
      fixture.begin();
      record(0, 0, receivers.size());
      fixture.finish();
      std::vector<bake::VisibilityResult> reference(receivers.size());
      transport.readVisibility(0, reference);
      for (size_t batch : {1u, 7u, 31u, 64u})
        for (size_t first = 0; first < receivers.size(); first += batch) {
          const auto count = std::min(batch, receivers.size() - first);
          const auto slot = uint32_t(first % 2);
          fixture.begin();
          record(slot, first, count);
          fixture.finish();
          std::vector<bake::VisibilityResult> measured(count);
          transport.readVisibility(slot, measured);
          require(std::memcmp(measured.data(), reference.data() + first, count * sizeof(bake::VisibilityResult)) == 0,
                  "Local/sun visibility and unit SH retain exact sample identity across batch sizes and slots");
        }
    }
    std::puts("Visibility batching: 127 receivers, local/sun SH, batches 1/7/31/64 and alternating slots match exactly");
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
  // Independent solid-angle reference for a sphere cut by the plane z=-1.
  // The receiver is at the origin. Count only the nearest emitting surface,
  // not the centre or the sphere's far side.
  double spherePlaneVisibility(double distance, double radius) {
    if (distance - radius >= 1)
      return 0;
    if (distance - radius * radius / distance <= 1)
      return 1; // Even a centre behind the plane can have a fully exposed silhouette.
    const double capRadiusSquared = radius * radius - (distance - 1) * (distance - 1);
    const double capCosine = 1 / std::sqrt(1 + capRadiusSquared);
    const double silhouetteCosine = std::sqrt(1 - radius * radius / (distance * distance));
    return (1 - capCosine) / (1 - silhouetteCosine);
  }
  void embeddedEmitterVisibility(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    constexpr std::array<float, 6> distances{.5f, 1.02f, 1.125f, 1.2f, 1.3f, 1.2505f};
    constexpr std::array<Vec3, 3> crossing{{{-.3f, -.3f, -1.2f}, {.3f, -.3f, -1.2f}, {0, .3f, -.8f}}};
    // Uniform barycentric quadrature of cos(emitter)/distance^2. The plane
    // intersects this triangle, so a centre-only test cannot describe it.
    double visible = 0, total = 0;
    constexpr uint32_t divisions = 512;
    for (uint32_t i = 0; i < divisions; ++i)
      for (uint32_t j = 0; i + j < divisions; ++j) {
        const double u = (i + 1. / 3) / divisions, v = (j + 1. / 3) / divisions;
        std::array<double, 3> p{};
        for (uint32_t c = 0; c < 3; ++c)
          p[c] = crossing[0][c] * (1 - u - v) + crossing[1][c] * u + crossing[2][c] * v;
        const double squared = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
        const double weight = 1 / std::pow(squared, 1.5); // Constant plane factor cancels in the ratio.
        total += weight;
        if (p[2] > -1)
          visible += weight;
      }
    const double triangleReference = visible / total;
    for (bool parallel : {false, true})
      for (float units : {.001f, 1.f, 1000.f}) {
        const auto blocker = world.wall(world.material({}), -units, 4 * units);
        auto source = world.scene({blocker});
        source->sceneScale = 1 / units; // Identical physical scene in three host unit systems.
        for (float distance : distances) {
          scene::SphereLight sphere;
          sphere.id = ++world.hash;
          sphere.position[2] = -distance * units;
          sphere.radius = .25f * units;
          sphere.radiance[0] = sphere.radiance[1] = sphere.radiance[2] = 1;
          source->lights.push_back(sphere);
        }
        for (float shift : {.5f, 0.f, -1.f}) {
          scene::SphereLight triangle;
          triangle.id = ++world.hash;
          triangle.triangle = 2; // Both sides emit; isolate geometric visibility.
          triangle.radiance[0] = triangle.radiance[1] = triangle.radiance[2] = 1;
          for (uint32_t c = 0; c < 3; ++c)
            for (uint32_t axis = 0; axis < 3; ++axis)
              triangle.corners[c][axis] = (crossing[c][axis] + (axis == 2 ? shift : 0)) * units;
          source->lights.push_back(triangle);
        }
        bake::Transport transport;
        bake::Options options;
        options.parallel = parallel;
        options.batchPoints = 9;
        auto job = pipelines.prepare("embedded-emitter-" + std::to_string(++world.hash), [&] {
          transport.initialize(fixture.physical, fixture.device, source, options);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!job->ready() && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(job->ready(), "Embedded emitter transport preparation completed");
        job->check();
        fixture.begin();
        transport.recordPrepare(fixture.command, world.resources);
        fixture.finish();
        std::array<bake::VisibilityPoint, 9> points{};
        for (uint32_t i = 0; i < points.size(); ++i)
          points[i].light = i;
        fixture.begin();
        transport.recordVisibility(fixture.command, 0, points, 4096, true);
        fixture.finish();
        std::array<bake::Result, 9> results;
        transport.read(0, results);
        std::printf("Embedded emitters: parallel=%u units=%g visibility", uint32_t(parallel), units);
        for (const auto &value : results)
          std::printf(" %.5f", value.irradiance[0][0]);
        std::puts("");
        require(results[5].irradiance[0][0] == 0,
                "A sphere fully enclosed just half a millimetre behind an opaque wall must not leak");
        for (uint32_t i = 0; i < distances.size(); ++i)
          require(std::abs(results[i].irradiance[0][0] - spherePlaneVisibility(distances[i], .25)) < .025,
                  "Embedded sphere visibility follows exposed solid angle, independent of centre placement and host units");
        require(results[0].irradiance[0][0] == 1 && results[4].irradiance[0][0] == 0 &&
                    results[6].irradiance[0][0] == 1 && results[8].irradiance[0][0] == 0 &&
                    std::abs(results[7].irradiance[0][0] - triangleReference) < .025,
                "Exposed emitters stay clear, enclosed emitters stay blocked, and crossing triangles match area quadrature");
        require(evaluate(results[0], {0, 0, -1}, 1) > 0 &&
                    evaluate(results[4], {0, 0, -1}, 1) == 0 &&
                    evaluate(results[5], {0, 0, -1}, 1) == 0 &&
                    evaluate(results[8], {0, 0, -1}, 1) == 0,
                "Direct irradiance uses the same embedded-emitter blockers as the mixed visibility mask");
      }
  }
  void emitterParity(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    auto source = world.scene({});
    scene::SphereLight sphere;
    sphere.id = ++world.hash;
    sphere.position[2] = -10;
    sphere.radius = .001f;
    sphere.radiance[0] = sphere.radiance[1] = sphere.radiance[2] = 1;
    source->lights.push_back(sphere);
    auto spot = sphere;
    spot.id = ++world.hash;
    spot.direction[2] = 1;
    spot.coneCos = .5f; // 60 degrees outer, 30 degrees inner
    spot.softness = .5f;
    source->lights.push_back(spot);
    auto surface = spot;
    surface.id = ++world.hash;
    surface.coneCos = 0;
    surface.softness = 0;
    surface.focus = 1;
    source->lights.push_back(surface);
    bake::Transport transport;
    bake::Options options;
    options.batchPoints = bake::Budget::maximumPoints;
    options.initialBatchPoints = 4;
    options.rayBudget = bake::Budget::maximumWork;
    auto job = pipelines.prepare("emitter-pathtrace-parity", [&] {
      transport.initialize(fixture.physical, fixture.device, source, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Emitter parity preparation completed");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    constexpr float diagonal = .70710678118f;
    const bake::VisibilityPoint points[] = {
        {{{0, 0, 0, 0}, {}}, 0}, {{{0, 0, 10, 0}, {}}, 0},
        {{{10 * diagonal, 0, -10 + 10 * diagonal, 0}, {}}, 0},
        {{{10 * diagonal, 0, -10 + 10 * diagonal, 0}, {}}, 1},
        {{{0, 0, 0, 0}, {}}, 2},
        {{{10 * diagonal, 0, -10 + 10 * diagonal, 0}, {}}, 2},
        {{{0, 0, -20, 0}, {}}, 2}};
    fixture.begin();
    transport.recordVisibility(fixture.command, 0, points, 256, true);
    fixture.finish();
    std::array<bake::Result, 7> results;
    transport.read(0, results);
    const float nearIrradiance = evaluate(results[0], {0, 0, -1}, 1);
    const float farIrradiance = evaluate(results[1], {0, 0, -1}, 1);
    const float radial = evaluate(results[2], {-diagonal, 0, -diagonal}, 1);
    const float shaped = evaluate(results[3], {-diagonal, 0, -diagonal}, 1);
    const float t = (diagonal - .5f) / (std::cos(std::numbers::pi_v<float> / 6) - .5f);
    const float expectedShape = t * t * (3 - 2 * t);
    require(nearIrradiance > 3e-8f && nearIrradiance < 3.6e-8f &&
                std::abs(farIrradiance / nearIrradiance - .25f) < .001f,
            "Subpixel emitters retain energy and inverse-square radial falloff instead of cancelling to zero");
    require(std::abs(radial / nearIrradiance - 1) < .001f && std::abs(shaped / radial - expectedShape) < .002f,
            "Spotlight penumbra uses the path tracer's angular inner/outer cone interpretation");
    require(std::abs(evaluate(results[4], {0, 0, -1}, 1) / nearIrradiance - 1) < .001f &&
                std::abs(evaluate(results[5], {-diagonal, 0, -diagonal}, 1) / radial - diagonal) < .001f &&
                evaluate(results[6], {0, 0, 1}, 1) == 0,
            "Texture-light cosine shaping preserves frontal energy, attenuates oblique light and emits nothing behind");
    fixture.begin();
    transport.recordVisibility(fixture.command, 0, points, 1, true);
    fixture.finish();
    std::array<bake::Result, 7> smallReference;
    transport.read(0, smallReference);
    const auto beforeLarge = transport.memoryBytes();
    std::vector<bake::VisibilityPoint> maximum(bake::Budget::maximumPoints - 3);
    for (size_t i = 0; i < maximum.size(); ++i) maximum[i] = points[i % 7];
    std::vector<bake::Result> maximumResults(maximum.size());
    fixture.begin();
    transport.recordVisibility(fixture.command, 0, maximum, 1, true);
    fixture.finish();
    transport.read(0, maximumResults);
    for (size_t i = 0; i < maximumResults.size(); ++i)
      require(std::memcmp(&maximumResults[i], &smallReference[i % 7], sizeof(bake::Result)) == 0,
              "Rectangular dispatches retain each distinct receiver and the partial final row exactly");
    require(transport.memoryBytes() - beforeLarge < uint64_t(maximum.size()) * 128,
            "Large compact visibility batches do not allocate full irradiance results per receiver");
    const auto compactBatch = transport.deviceBatch(0);
    require(uint64_t(compactBatch.capacity) * sizeof(bake::Result) <= compactBatch.results.range,
            "GPU producers receive the common point/full-result capacity after compact CPU work");
    bool undersizedRefused = false;
    try { transport.recordDeviceRadiance(VK_NULL_HANDLE, 0, compactBatch.capacity + 1, 1); }
    catch (const std::invalid_argument &) { undersizedRefused = true; }
    require(undersizedRefused, "A GPU producer cannot overrun compact result storage");
    // Reuse that completed slot for a full result larger than its compact
    // allocation. Point capacity alone must not suppress result-buffer growth.
    std::vector<bake::Point> fullPoints(maximum.size(), points[0].surface);
    fixture.begin();
    transport.recordBatch(fixture.command, 0, fullPoints, 1, 1);
    fixture.finish();
    transport.read(0, maximumResults);
    for (const auto &value : maximumResults)
      require(value.statistics[2] == 1 && std::isfinite(value.irradiance[0][0]),
              "A completed compact slot grows its full irradiance storage before reuse");
    std::printf("Emitter parity: near %.8g, twice-distance %.8g, cone %.5f (expected %.5f); %zu groups\n",
                  nearIrradiance, farIrradiance, shaped / radial, expectedShape, maximum.size());
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
    bake::Result referenceRay{};
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
      if (mode == 0) referenceRay = results.front();
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
    std::vector<bake::Point> padded(512);
    for (auto &ray : padded) {
      ray.position = {0, 0, 0, 0};
      ray.normal = {0, 0, 1, -1};
    }
    padded.front().normal[3] = 0;
    fixture.begin();
    transport.recordRadiance(fixture.command, 0, padded, 1);
    fixture.finish();
    std::vector<bake::Result> paddedResults(512);
    transport.read(0, paddedResults);
    const bake::Result empty{};
    require(std::memcmp(&referenceRay, &paddedResults.front(), sizeof(empty)) == 0,
            "Skipping inactive dielectric rays leaves active radiance unchanged");
    for (size_t i = 1; i < paddedResults.size(); ++i)
      require(std::memcmp(&empty, &paddedResults[i], sizeof(empty)) == 0,
              "Inactive rays inside a measured body produce empty results without transport");
    // Match the CPU-upload/readback result through GPU-only batch buffers.
    // Transfers here are test producers/consumers; live reflections never copy
    // these points or results across the host boundary.
    bake::Transport deviceTransport;
    options.cpuReadback = false;
    options.deviceLocalBatches = true;
    auto deviceJob = pipelines.prepare("device-local-dielectric-transport", [&] {
      deviceTransport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deviceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!deviceJob->ready() && std::chrono::steady_clock::now() < deviceDeadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(deviceJob->ready(), "Device-local transport preparation completed");
    deviceJob->check();
    Buffer upload, readback;
    upload.create(fixture.physical, fixture.device, padded.size() * sizeof(bake::Point),
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    upload.write(padded.data(), padded.size() * sizeof(bake::Point));
    readback.create(fixture.physical, fixture.device, paddedResults.size() * sizeof(bake::Result),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
    for (uint32_t frame = 0; frame < gpuFrameCount + 2; ++frame) {
      const auto slot = frame % gpuFrameCount;
      fixture.begin();
      if (frame == 0) deviceTransport.recordPrepare(fixture.command, world.resources);
      const auto batch = deviceTransport.deviceBatch(slot);
      const VkBufferCopy inputCopy{0, 0, upload.capacity};
      vkCmdCopyBuffer(fixture.command, upload.handle, batch.points.buffer, 1, &inputCopy);
      VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
      vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      deviceTransport.recordDeviceRadiance(fixture.command, slot, uint32_t(padded.size()), 1);
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      const VkBufferCopy outputCopy{0, 0, readback.capacity};
      vkCmdCopyBuffer(fixture.command, batch.results.buffer, readback.handle, 1, &outputCopy);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      fixture.finish();
      std::vector<bake::Result> copied(paddedResults.size());
      readback.read(copied.data(), copied.size() * sizeof(bake::Result));
      require(std::memcmp(copied.data(), paddedResults.data(), copied.size() * sizeof(bake::Result)) == 0,
              "Device-local batch transport matches host batches across reused frame slots");
    }
    bool refusedHostBatch = false, refusedHostRead = false;
    try { deviceTransport.recordRadiance(VK_NULL_HANDLE, 0, padded, 1); }
    catch (const std::logic_error &) { refusedHostBatch = true; }
    try { deviceTransport.read(0, paddedResults); }
    catch (const std::logic_error &) { refusedHostRead = true; }
    require(refusedHostBatch && refusedHostRead,
            "GPU-only transport rejects accidental CPU access before recording or mapping");
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
  void modulatingDecals(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaBlendEnabled = 1;
    blend.alphaTestCompareOp = 7;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 4;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 2;
    blend.writeMask = 15;
    const auto base = world.wall(world.material({.diffuse = .8f}), 1, 1000);
    const auto tint = world.material({.diffuse = .25f});
    const auto first = world.wall(tint, .9975f, 1000, &blend, 0xffffffff, true);
    const auto second = world.wall(tint, .996f, 1000, &blend, 0xffffffff, true);
    const auto detached = world.wall(tint, .95f, 1000, &blend, 0xffffffff, true);
    auto lit = [&](std::initializer_list<uint64_t> instances) {
      auto scene = world.scene(instances);
      scene->sun.isDistant = true;
      std::fill_n(scene->sun.direction, 3, 0);
      scene->sun.direction[2] = 1;
      std::fill_n(scene->sun.radiance, 3, 1);
      return run(fixture, pipelines, world, scene, 1).irradiance[0][0];
    };
    const float plain = lit({base}), darkened = lit({base, first}), stacked = lit({base, first, second}),
                floating = lit({base, detached});
    require(plain > .01f && std::abs(darkened / plain - .5f) < .015f,
            "An attached modulating decal changes the receiver's bounce colour without blocking its sun");
    require(std::abs(stacked / plain - .25f) < .015f,
            "Independent attached modulating layers multiply without creating extra bounce surfaces");
    require(std::abs(floating / plain - 1) < .015f,
            "A detached decal neither tints the remote surface nor casts a fake shadow");
    world.texture(0x7101, {64, 64, 64, 0, 128, 128, 128, 0});
    const auto textured = world.wall(world.material({.diffuse = 1, .albedo = L"0x7101"}),
                                     .9975f, 1000, &blend, 0xffffffff, true);
    const float patterned = lit({base, textured});
    require(std::abs(patterned / plain - .75f) < .04f,
            "Decal transport samples its colour texture while modulation remains independent of alpha");
    auto sheet = world.scene({first});
    sheet->dome.enabled = true;
    sheet->dome.radiance = {1, 1, 1};
    const float passing = evaluate(run(fixture, pipelines, world, sheet, 1), {0, 0, 1}, 0);
    require(std::abs(passing - std::numbers::pi_v<float>) < .05f,
            "Modulating decals alone leave environment visibility unchanged");
    const auto emitter = world.wall(world.material({.emission = 3}), 1, 1000);
    auto glowing = world.scene({emitter, first});
    bake::Transport transport;
    auto job = pipelines.prepare("decal-emission-diagonal", [&] {
      transport.initialize(fixture.physical, fixture.device, glowing);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Decal directional capture preparation completed");
    job->check();
    const bake::Point rays[] = {{{0, 0, 0, 0}, {0, 0, 1, 0}}, {{.1f, 0, 0, 0}, {0, 0, 1, 0}}};
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    transport.recordRadiance(fixture.command, 0, rays, 1);
    fixture.finish();
    bake::Result captured[2];
    transport.read(0, captured);
    for (const auto &value : captured)
      require(std::abs(value.irradiance[0][0] - 1.5f) < .001f &&
                  std::abs(value.irradiance[0][3] - 1) < .001f,
              "Decal modulation applies once at shared triangle edges and preserves emitter hit distance");
    std::printf("Modulating decals: single %.4f, stacked %.4f, detached %.4f, textured %.4f; visibility %.4f\n",
                darkened / plain, stacked / plain, floating / plain, patterned / plain, passing);
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
  void captureNoise(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    const auto receiver = world.wall(world.material({.diffuse = .5f}), 1, 2);
    auto scene = world.scene({receiver});
    bake::Transport transport;
    bake::Options options;
    options.batchPoints = 512;
    options.textureResolution = 16;
    auto job = pipelines.prepare("bake-cube-noise-" + std::to_string(world.revision), [&] {
      transport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Cubemap variance fixture preparation completed");
    job->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    std::vector<TransportLight> lights(100);
    for (auto &light : lights) {
      light.positionRadius = {0, 0, 0, .4f};
      light.directionCone[3] = -1;
    }
    lights[0].radiance = {5, 5, 5, 0};
    transport.updateLighting(0, lights, scene->sun, scene->dome, 100);
    std::vector<bake::Point> rays;
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 32; ++x)
        rays.push_back({{(float(x) + .5f) * .4f / 32 - .2f,
                          (float(y) + .5f) * .4f / 16 - .2f, 0, 0}, {0, 0, 1, 0}});
    std::vector<bake::Result> result(rays.size());
    double error[2]{}, means[2]{}, expectedMean = 0;
    for (uint32_t quality = 0; quality < 2; ++quality) {
      fixture.begin();
      transport.recordRadiance(fixture.command, 0, rays, 1, quality ? 256 : 1);
      fixture.finish();
      transport.read(0, result);
      for (size_t i = 0; i < rays.size(); ++i) {
        // A sphere wholly inside the receiver hemisphere has irradiance
        // pi*r^2*cos(axis)/distance^2. Lambertian albedo/pi cancels pi.
        const double z = 1 - options.bias;
        const double distance2 = z * z + rays[i].position[0] * rays[i].position[0] +
                                 rays[i].position[1] * rays[i].position[1];
        const double expected = .5 * 5 * .4 * .4 * z / std::pow(distance2, 1.5);
        error[quality] += std::pow(result[i].irradiance[0][0] - expected, 2);
        means[quality] += result[i].irradiance[0][0];
        if (!quality) expectedMean += expected;
      }
    }
    require(error[1] < error[0] * .1, "Offline cubemap samples reduce error against analytic direct lighting");
    require(std::abs(means[1] / expectedMean - 1) < .15,
            "Cubemap averaging retains expected lighting energy, rather than suppressing noisy light");
    std::printf("Cubemap RMSE %.5f -> %.5f; mean %.5f -> %.5f, analytic %.5f\n",
                std::sqrt(error[0] / rays.size()), std::sqrt(error[1] / rays.size()),
                means[0] / rays.size(), means[1] / rays.size(), expectedMean / rays.size());
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
    std::vector<bake::Result> geometry(rays.size());
    const auto geometryBytes = transport.memoryBytes();
    fixture.begin();
    transport.recordGeometry(fixture.command, 0, rays);
    fixture.finish();
    transport.read(0, geometry);
    for (size_t i = 0; i < rays.size(); ++i) {
      require(std::abs(geometry[i].irradiance[0][3] - results[i].irradiance[0][3]) < 1e-5f &&
                  geometry[i].statistics[0] == results[i].statistics[0] &&
                  geometry[i].statistics[1] == results[i].statistics[1] && geometry[i].statistics[2] == 1,
              "Geometry-only candidate rays match captured first-hit distance, backface and explicit miss flags");
      require(geometry[i].irradiance[0][0] == 0 && geometry[i].irradiance[0][1] == 0 &&
                  geometry[i].irradiance[0][2] == 0,
              "Candidate geometry queries do not evaluate lighting");
    }
    require(transport.memoryBytes() == geometryBytes,
            "Geometry-only candidate queries reuse the prepared transport buffers and acceleration structure");
    const bake::Point axial[] = {{{0, 0, 0, 0}, {0, 0, 4, 0}},
                                 {{0, 0, 0, 0}, {0, 0, -4, 0}}};
    fixture.begin();
    transport.recordGeometry(fixture.command, 0, axial);
    fixture.finish();
    bake::Result axialResults[2];
    transport.read(0, axialResults);
    require(std::abs(axialResults[0].irradiance[0][3] - 1) < 1e-6f &&
                axialResults[0].statistics[1] == 0 && axialResults[1].statistics[1] == 1 &&
                std::abs(axialResults[1].irradiance[0][3] - options.distance / scene->sceneScale) < 1e-5f,
            "Geometry rays normalize directions without origin bias and distinguish a hit from a distance-cap miss");
    bool geometryRefused = false;
    const bake::Point zeroDirection{};
    try {
      transport.recordGeometry(fixture.command, 0, {&zeroDirection, 1});
    } catch (const std::invalid_argument &) {
      geometryRefused = true;
    }
    require(geometryRefused, "Geometry queries reject zero directions before recording commands");
    std::printf("Candidate geometry: %zu first-hit distances/flags match radiance; explicit miss, "
                "unit directions and prepared-buffer reuse passed\n", rays.size());
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
    auto inset = data;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      inset.header.probes[0].minimum[axis] += .001f;
      inset.header.probes[0].maximum[axis] -= .001f;
    }
    require(bool(probes::sample(inset, {0, 0, -1}, {0, 0, 1}, .2f)),
            "A floor receiver can use influence reaching beyond its inset projection proxy");
    test::probeQueries(fixture, inset);
    probes::Capture floorCaptures[] = {capture, capture};
    floorCaptures[0].position = {-.5f, 0, 0};
    floorCaptures[1].position = {.5f, 0, 0};
    for (auto &pixel : floorCaptures[0].radiance)
      pixel = {4, 0, 0, 0};
    for (auto &pixel : floorCaptures[1].radiance)
      pixel = {0, 2, 0, 0};
    auto floorData = probes::filter(floorCaptures, 64);
    test::probeQueries(fixture, floorData);
    floorData.header.probes[0].position[3] = 1;
    floorData.header.probes[1].position[3] = .05f;
    test::probeQueries(fixture, floorData);
    floorData.header.probes[0].position[3] = 0;
    floorData.header.probes[1].position[3] = 0;
    test::probeQueries(fixture, floorData);
    std::vector<probes::Capture> allSlots(probes::maxProbes, capture);
    for (uint32_t i = 0; i < allSlots.size(); ++i) {
      auto &slot = allSlots[i];
      const float offset = i + 1 == allSlots.size() ? 0.f : 10.f * (i + 1);
      slot.position[0] += offset;
      slot.bounds.minimum[0] += offset;
      slot.bounds.maximum[0] += offset;
      for (auto &pixel : slot.radiance)
        pixel = {float(i + 1), 0, 0, 0};
    }
    const auto lastSlot = probes::filter(allSlots, 64);
    const auto lastValue = probes::sample(lastSlot, {0, 0, 0}, {0, 0, 1}, .5f);
    require(lastValue && std::abs((*lastValue)[0] - probes::maxProbes) < .001f,
            "The final supported cubemap slot keeps its independent metadata and texel offset");
    test::probeQueries(fixture, lastSlot);
    capture.influence.include({-2, -2, -2});
    capture.influence.include({2, 2, 2});
    const auto extended = probes::filter({&capture, 1}, 64);
    for (uint32_t skyCase = 1; skyCase <= 3; ++skyCase)
      test::probeQueries(fixture, extended, skyCase);
    test::probeQueries(fixture, extended, 4); // bounded probe relocation
    test::probeQueries(fixture, extended, 0, true);
    test::probeQueries(fixture, extended, 4, true);
    TransportLight lamp;
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
    // Separate dispatch reuse from acceleration-structure construction. The
    // fixture has an emissive surface, finite local emitter and environment;
    // compare full indirect SH/moments as well as reflection radiance.
    std::array<std::vector<bake::Result>, 2> retainedReference;
    auto measure = [&](bake::Transport &candidate, uint32_t slot, bool radiance) {
      candidate.updateLighting(slot, {&lamp, 1}, scene->sun, scene->dome, 100);
      fixture.begin();
      if (radiance)
        candidate.recordRadiance(fixture.command, slot, rays, 3, 65);
      else
        candidate.recordBatch(fixture.command, slot, rays, 65, 3);
      fixture.finish();
      std::vector<bake::Result> measured(rays.size());
      candidate.read(slot, measured);
      return measured;
    };
    for (uint32_t mode = 0; mode < 2; ++mode)
      retainedReference[mode] = measure(transport, 0, mode != 0);
    for (uint32_t build = 0; build < 3; ++build) {
      bake::Transport rebuilt;
      bake::Transport *candidate = &transport;
      if (build) {
        auto rebuild = pipelines.prepare("transport-repeat-build-" + std::to_string(build), [&] {
          rebuilt.initialize(fixture.physical, fixture.device, scene, options);
        });
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!rebuild->ready() && std::chrono::steady_clock::now() < limit)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(rebuild->ready(), "Repeated acceleration structure preparation completed");
        rebuild->check();
        fixture.begin();
        rebuilt.recordPrepare(fixture.command, world.resources);
        fixture.finish();
        rebuilt.releaseBuildInputs();
        candidate = &rebuilt;
      }
      for (uint32_t slot : {0u, 1u, 0u})
        for (uint32_t mode = 0; mode < 2; ++mode) {
          const auto measured = measure(*candidate, slot, mode != 0);
          require(std::memcmp(measured.data(), retainedReference[mode].data(),
                              measured.size() * sizeof(bake::Result)) == 0,
                  "Indirect SH, moments and radiance repeat exactly on retained and rebuilt acceleration structures");
        }
    }
    std::puts("Transport repeatability: retained and two rebuilt acceleration structures, 65 samples, "
              "indirect SH/moments and radiance, slots 0/1/0 match exactly");
    for (uint32_t samples : {16u, 65u}) {
      fixture.begin();
      transport.recordRadiance(fixture.command, 0, rays, 2, samples);
      fixture.finish();
      transport.read(0, reference);
      std::vector<Vec3> average(rays.size());
      for (uint32_t ordinal = 0; ordinal < samples; ++ordinal) {
        fixture.begin();
        transport.recordRadiance(fixture.command, 0, rays, 2, 1, ordinal);
        fixture.finish();
        transport.read(0, results);
        for (size_t ray = 0; ray < rays.size(); ++ray)
          for (uint32_t c = 0; c < 3; ++c)
            average[ray][c] += results[ray].irradiance[0][c] / float(samples);
      }
      for (size_t ray = 0; ray < rays.size(); ++ray) {
        require(reference[ray].statistics[2] == float(samples), "Radiance reports its actual averaged lighting sample count");
        for (uint32_t c = 0; c < 3; ++c)
          require(std::abs(reference[ray].irradiance[0][c] - average[ray][c]) < 1e-5f,
                  "Cubemap lighting averages independent stable sample identities instead of repeating one path");
      }
    }
    auto inactive = rays.front();
    inactive.normal[3] = -1;
    fixture.begin();
    transport.recordRadiance(fixture.command, 0, {&inactive, 1}, 2, 16);
    fixture.finish();
    bake::Result skipped;
    transport.read(0, {&skipped, 1});
    require(skipped.irradiance[0] == Vec4{} && skipped.statistics == Vec4{},
            "Inactive radiance points retain zero output and metadata under parallel sampling");
    std::printf("Cubemap readback: +Z red %.3f at %.3f units, -Z blue %.3f; "
                "CPU/GPU box projection agrees\n",
                positiveZ[0], positiveZ[3], negativeZ[2]);
  }
} // namespace
int main() try {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {
    test::Fixture fixture;
    {
      Buffer staging;
      staging.create(fixture.physical, fixture.device, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
      std::array<uint8_t, 16> expected{};
      staging.write(expected.data(), expected.size());
      const uint8_t region[] = {1, 2, 3, 4};
      staging.write(region, sizeof(region), 5);
      std::copy(std::begin(region), std::end(region), expected.begin() + 5);
      std::array<uint8_t, 16> read{};
      staging.read(read.data(), read.size());
      require(read == expected, "Staging offset writes preserve neighbouring upload regions");
      bool rejected = false;
      try {
        staging.write(region, sizeof(region), 14);
      } catch (VkResult error) {
        rejected = error == VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      require(rejected, "Staging offset writes reject a range past the allocation");
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(fixture.physical, &properties);
    pipeline::Service pipelines(fixture.device, properties, false);
    World world(fixture);
    if (std::getenv("REMIX_BENCH_VISIBILITY")) {
      bakeVisibility(fixture, pipelines, world);
      return 0;
    }
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
    auto styledScene = world.scene({patch}, {lamp});
    for (auto &light : styledScene->lights) light.style = 32;
    const auto styledIndirect = run(fixture, pipelines, world, styledScene, 1);
    for (const auto &coefficient : styledIndirect.irradiance)
      require(coefficient[0] == 0 && coefficient[1] == 0 && coefficient[2] == 0,
              "Styled energy cannot leave permanent shared indirect SH after switch-off");

    const auto shade = world.quad(world.material({}), {{{1, -3, -.5f}, {1, 3, -.5f}, {1, 3, 2}, {1, -3, 2}}});
    const auto shadowed = run(fixture, pipelines, world, world.scene({patch, shade}, {lamp}), 1);
    require(lit.irradiance[0][0] > .01f && shadowed.irradiance[0][0] < lit.irradiance[0][0] * .05f,
            "Hardware visibility shadows local-light indirect radiance without "
            "hiding the receiver patch");
    captureProbe(fixture, pipelines, world);
    captureNoise(fixture, pipelines, world);
    siteValidation(fixture, pipelines, world);
    dielectricTransport(fixture, pipelines, world);
    blendedCoverage(fixture, pipelines, world);
    modulatingDecals(fixture, pipelines, world);
    bakeSurface(fixture, pipelines, world);
    bakeVisibility(fixture, pipelines, world);
    overflowVisibility(fixture, pipelines, world);
    localProbeVisibilityTests(fixture, pipelines, world);
    embeddedEmitterVisibility(fixture, pipelines, world);
    emitterParity(fixture, pipelines, world);
    lightSelectionVariance(fixture, pipelines, world);
    aggregateDiffuse(fixture, pipelines, world);
    test::lightingComponents(fixture, pipelines, world);
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
