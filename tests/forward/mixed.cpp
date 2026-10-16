#include "forward/lightmap/data.h"
#include "forward/mixed/data.h"
#include "forward/ray/capture.h"
#include "scene/fixtures.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
using namespace remix;
using namespace remix::test;
namespace m = remix::forward::mixed;
namespace lm = remix::forward::lightmap;
int main() {
  NativeResources resources;
  const auto mat = material(resources, 1, 1, 1, 1);
  const auto mesh = quad(resources, 2, mat);
  const auto handle = resources.createRetained(instance(mesh));
  auto source = std::make_shared<scene::BakeScene>();
  source->ready = true;
  source->draws = resources.retained(handle)->records;
  source->charts.push_back({0, 0, 5, 5, {{0, 0}, {4, 4}, {4, 0}, {0, 0}, {0, 4}, {4, 4}}});
  for (uint64_t id : {50u, 10u, 40u, 20u, 30u}) {
    scene::SphereLight light;
    light.id = id;
    light.radius = .25f;
    light.position[2] = -2;
    light.radiance[0] = light.radiance[1] = light.radiance[2] = 1;
    source->lights.push_back(light);
  }
  scene::freezeBakeScene(*source);
  const auto layout = lm::plan(*source, {16, 2, 2, 512});
  const auto plan = m::plan(source, layout);
  require(plan.overflow == 1 && plan.tasks.size() == layout.points.size() * 4,
          "Mixed light storage is bounded per chart and explicitly counts "
          "overlap overflow");
  for (uint32_t channel = 0; channel < 4; ++channel)
    require(source->lights[plan.charts[0][channel]].id == (channel + 1) * 10,
            "Equal-importance lights use stable identities for deterministic "
            "channel selection");
  for (const auto &task : plan.tasks)
    require(task.light == plan.charts[layout.pointCharts[task.point]][task.channel],
            "Each visibility task retains its exact light and surface-channel "
            "provenance");
  auto reordered = std::make_shared<scene::BakeScene>(*source);
  std::reverse(reordered->lights.begin(), reordered->lights.end());
  const auto reorderedPlan = m::plan(reordered, layout);
  for (uint32_t channel = 0; channel < 4; ++channel)
    require(reordered->lights[reorderedPlan.charts[0][channel]].id ==
                source->lights[plan.charts[0][channel]].id,
            "Captured submission order cannot change equal-importance light "
            "ownership");
  require(plan.lights.match(source->lights[0]) == m::none,
          "Unassigned overflow lights avoid a needless runtime mask lookup");
  {
    auto refined = plan;
    std::vector<forward::Vec4> visibility(layout.points.size());
    require(m::refine(refined, layout, visibility) && refined.overflow == 0 &&
                refined.tasks.size() == layout.points.size() && refined.charts[0][0] == 0,
            "Fully blocked channels promote the next candidate without expanding filtered storage");
    for (uint32_t channel = 0; channel < 4; ++channel)
      require(refined.occlusion.contains(0, plan.charts[0][channel]) &&
                  refined.lights.match(source->lights[plan.charts[0][channel]]) == plan.charts[0][channel],
              "Discarded zero channels retain exact light identity in compact static occlusion");
    require(refined.lights.match(source->lights[0]) == 0 && !refined.occlusion.contains(0, 0),
            "Promoted lights receive bindings without inheriting another light's occlusion");
    for (auto &value : visibility) value[0] = .25f;
    require(!m::refine(refined, layout, visibility) && refined.charts[0][0] == 0,
            "A visible promoted light retains its filtered channel");
    auto partial = plan;
    visibility.assign(layout.points.size(), {});
    visibility[0][0] = 1e-6f;
    m::refine(partial, layout, visibility);
    require(partial.charts[0][0] == plan.charts[0][0] &&
                !partial.occlusion.contains(0, plan.charts[0][0]),
            "Even one partially visible sample prevents a chart-wide blocked classification");
    auto unused = plan;
    auto noSamples = layout;
    noSamples.points.clear();
    noSamples.pointCharts.clear();
    require(!m::refine(unused, noSamples, {}) && unused.occlusion.count() == 0,
            "A chart without samples cannot be classified as blocked");
    auto clear = plan;
    visibility.assign(layout.points.size(), {0, .5f, 1, .25f});
    std::vector<lm::Direct> scratch(layout.points.size()), retained(layout.points.size());
    for (uint32_t point = 0; point < scratch.size(); ++point)
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        scratch[point][coefficient] = {0, float(point + coefficient + 11),
                                      float(point + coefficient + 22), float(point + coefficient + 33)};
    require(m::refine(clear, layout, visibility, {}, scratch, retained) &&
                clear.charts[0][0] == 0 && clear.charts[0][2] == m::none &&
                clear.occlusion.state(0, plan.charts[0][2]) == m::Visibility::Clear &&
                clear.directCharts[0] == forward::bake::UInt4{plan.charts[0][1], plan.charts[0][2],
                                                            plan.charts[0][3], m::none},
            "Clear masks free filtered storage while retaining independent positive-light SH owners");
    for (uint32_t point = 0; point < scratch.size(); ++point)
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        require(retained[point][coefficient] == forward::Vec4{float(point + coefficient + 11),
                                                            float(point + coefficient + 22),
                                                            float(point + coefficient + 33), 0},
                "Every signed directional coefficient follows its light when working channels are compacted");
    for (auto &value : visibility) value[0] = 1;
    for (auto &point : scratch) for (auto &coefficient : point) coefficient[0] = -7;
    require(!m::refine(clear, layout, visibility, {}, scratch, retained) &&
                clear.directCharts[0][3] == 0 && clear.occlusion.clearCount() == 2 &&
                retained[0][8] == forward::Vec4{19, 30, 41, -7},
            "Later clear candidates fill spare direct channels without overwriting previously retained SH");
    auto almostClear = plan;
    visibility.assign(layout.points.size(), {1, 1, 1, 1});
    visibility[0][0] = .999999f;
    m::refine(almostClear, layout, visibility);
    require(almostClear.charts[0][0] == plan.charts[0][0] &&
                almostClear.occlusion.state(0, plan.charts[0][0]) == m::Visibility::Unknown,
            "Even one nonunit sample prevents clear classification");
    auto geometry = lm::meshes(*source, layout, {}, clear.charts, clear.directCharts);
    for (const auto &vertex : geometry[0].vertices)
      require(vertex.lights == clear.charts[0] && vertex.directLights == clear.directCharts[0],
              "Geometry carries distinct filtered visibility and unit direct-light identities");
    m::Occlusion mask(2, 64);
    for (uint32_t light : {0u, 3u, 4u, 7u, 8u, 11u, 12u, 15u, 16u, 31u, 32u, 63u}) mask.mark(0, light);
    for (uint32_t light : {1u, 5u, 9u, 13u, 17u, 33u, 62u}) mask.mark(0, light, m::Visibility::Clear);
    mask.mark(1, 5);
    mask.mark(1, 5);
    const auto pixels = mask.pixels(4);
    require(mask.count() == 13 && mask.clearCount() == 7 && mask.contains(1, 5) && !mask.contains(0, 5) &&
                mask.state(0, 5) == m::Visibility::Clear && mask.state(2, 0) == m::Visibility::Unknown &&
                pixels.width == 4 && pixels.height == 3 &&
                pixels.rgba[8] == 0x5490 && pixels.rgba[9] == 0x5490 && pixels.rgba[10] == 0x5490 &&
                pixels.rgba[11] == 0x5490 && pixels.rgba[12] == 0x4880 && pixels.rgba[15] == 0x5400 &&
                pixels.rgba[16] == 0x4880 && pixels.rgba[23] == 0x5600 && pixels.rgba[25] == 0x4400,
            "Two-bit states preserve chart boundaries, all byte lanes and 16-light word seams");
  }
  auto changed = source->lights[1];
  changed.radiance[0] = 20;
  require(plan.lights.match(changed) == 1, "Colour changes retain geometry-valid static visibility");
  changed.position[0] += .01f;
  require(plan.lights.match(changed) == m::none, "Moving a light rejects its old static mask");
  changed = source->lights[1];
  changed.radius *= 2;
  require(plan.lights.match(changed) == m::none, "Changing emitter radius rejects its old penumbra");
  changed = source->lights[1];
  changed.dynamic = true;
  require(plan.lights.match(changed) == m::none, "Dynamic lights cannot inherit a static light channel");
  changed = source->lights[1];
  ++changed.id;
  require(plan.lights.match(changed) == m::none,
          "Equivalent geometry under another light identity cannot steal a mask");
  auto geometry = lm::meshes(*source, layout, {}, plan.charts);
  for (const auto &vertex : geometry[0].vertices)
    require(vertex.lights == plan.charts[0], "Light ownership accompanies chart geometry as discrete integer "
                                             "attributes");
  std::vector<forward::bake::Result> results(layout.points.size());
  std::vector<forward::Vec4> visibility(layout.points.size(), {0, .5f, 1, .25f});
  const auto packed = lm::pixels(layout, results, {}, visibility);
  const size_t pageTexels = 16 * 16, pixel = layout.addresses[0];
  const size_t index = (lm::irradianceLayers * pageTexels + pixel) * 4;
  require(packed[index] == 0 && packed[index + 1] == 0x3800 && packed[index + 2] == 0x3c00 &&
              packed[index + 3] == 0x3400,
          "Visibility layers preserve independent zero, soft, full and "
          "quarter-strength channels");
  require(packed[(lm::irradianceLayers * pageTexels) * 4 + 1] == 0x3800,
          "Chart padding keeps the same light-channel ownership and visibility "
          "values");
  std::vector<lm::Direct> direct(layout.points.size());
  for (auto &point : direct) {
    point[0] = {1, 2, 3, 4};
    point[1] = {-1, -.5f, .25f, 0};
  }
  const auto withDirect = lm::pixels(layout, results, {}, visibility, direct);
  const size_t directPixel = (lm::directLayer * pageTexels + pixel) * 4;
  require(withDirect[directPixel] == 0x3c00 && withDirect[directPixel + 1] == 0x4000 &&
              withDirect[directPixel + 2] == 0x4200 && withDirect[directPixel + 3] == 0x4400 &&
              withDirect[directPixel + pageTexels * 4] == 0xbc00 && withDirect[index] == 0,
          "Signed per-light direct coefficients retain channel ownership "
          "separately from static visibility");
  require(withDirect[lm::directLayer * pageTexels * 4] == 0x3c00,
          "Direct diffuse padding reuses the chart-local donor");
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try {
    m::plan(source, layout, .001f, stop.get_token());
  } catch (const std::runtime_error &) {
    cancelled = true;
  }
  require(cancelled, "Superseded mixed light planning supports cancellation");
  auto capture = [&](std::span<const scene::Draw> current, double seconds = 0) {
    return forward::ray::capture(source.get(), current, source->lights, source->sun, source->dome, 1,
                                 seconds);
  };
  auto frozen = capture({});
  require(frozen.inputs.triangles.size() == 2 && frozen.key == capture({}).key,
          "Live rays retain complete authored geometry with a stable transport "
          "cache key");
  auto streamed = source->lights;
  streamed[0].position[0] += 2;
  streamed[0].radiance[0] += 5;
  const auto relit = forward::ray::capture(source.get(), {}, streamed, source->sun, source->dome, 1);
  require(relit.key == frozen.key &&
              std::any_of(relit.inputs.lights.begin(), relit.inputs.lights.end(),
                          [](const auto &light) { return light.positionRadius[0] == 2; }),
          "Analytic light edits stream fresh data without invalidating "
          "geometry/atlas reuse");
  auto current = resources.retained(handle)->records[0];
  current.model[12] = 2;
  auto moved = capture({&current, 1});
  require(moved.inputs.triangles.size() == 2 && moved.key != frozen.key &&
              std::abs(moved.inputs.vertices[0].position[0] - 1.2f) < 1e-5f,
          "A live retained transform replaces its authored counterpart instead "
          "of creating a ghost");
  current.blend.enabled = true;
  const auto blended = capture({&current, 1});
  require(blended.inputs.triangles.size() == 2 && blended.key != moved.key &&
              (blended.inputs.materials[0].flags[3] & forward::bake::sourceOverCoverageFlag),
          "Live source-over sheets retain coverage transport without "
          "duplicating authored geometry");
  current.blend.src = current.blend.dst = 1; // additive effects remain compatibility-only
  require(capture({&current, 1}).inputs.triangles.empty(),
          "A current compatibility-only instance suppresses its old authored "
          "ray geometry");
  current.material = std::make_shared<scene::Material>(*current.material);
  current.material->transmission = current.material->bakeSmoothBoundary = true;
  current.material->transmittanceDistance = 1;
  current.material->refractiveIndex = 1.333f;
  require(capture({&current, 1}).inputs.triangles.empty(),
          "A smooth optical policy cannot turn an additive effect into transport geometry");
  current.blend.enabled = false;
  const auto boundary = capture({&current, 1});
  require(boundary.inputs.triangles.size() == 2 &&
              (boundary.inputs.materials[0].flags[3] & forward::bake::dielectricBoundaryFlag),
          "Live ray capture includes explicit smooth optical boundaries");
  current = resources.retained(handle)->records[0];
  current.instanceId = 901;
  current.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
  require(capture({&current, 1}).inputs.triangles.size() == 4,
          "Current dynamic actors augment complete static ray geometry");
  current.surface = std::make_shared<scene::Surface>(*current.surface);
  current.surface->bonesPerVertex = 1;
  current.surface->bones.assign(4, 0);
  current.surface->weights.assign(4, 1);
  current.bones = std::make_shared<std::vector<scene::Matrix>>(1, scene::identity());
  auto posed = capture({&current, 1});
  const size_t actorVertex = posed.source->draws[0].instanceId == 901 ? 0 : 4;
  (*current.bones)[0][12] = 1;
  auto reposed = capture({&current, 1});
  require(posed.key != reposed.key && std::abs(reposed.inputs.vertices[actorVertex].position[0] -
                                               posed.inputs.vertices[actorVertex].position[0] - 1) < 1e-5f,
          "Live acceleration inputs follow current bone transforms");
  current.material = std::make_shared<scene::Material>(*current.material);
  current.material->columns = 2;
  current.material->fps = 1;
  auto firstFrame = capture({&current, 1}, 0), secondFrame = capture({&current, 1}, 1);
  require(firstFrame.key != secondFrame.key &&
              std::abs(secondFrame.inputs.vertices[actorVertex].uv[0] -
                       firstFrame.inputs.vertices[actorVertex].uv[0] - .5f) < 1e-5f,
          "Live material animation carries its UV frame offset into ray-hit "
          "shading");
  current.material->texture = std::make_shared<scene::Texture>();
  current.material->texture->updates = std::make_shared<scene::TextureUpdates>();
  const auto textureKey = capture({&current, 1}).key;
  ++current.material->texture->updates->revision;
  require(textureKey != capture({&current, 1}).key,
          "Dynamic texture revisions invalidate the sampled ray material atlas");
  std::puts("Mixed chart/light ownership, bounded overlap, shape invalidation "
            "and visibility channels passed");
}
