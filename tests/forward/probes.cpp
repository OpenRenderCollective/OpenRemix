#include "forward/probes/data.h"
#include "platform/workers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace remix::forward;
namespace p = remix::forward::probes;
void testCaptureWork();
void testProbeImageData();
namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  p::Capture capture(uint32_t resolution = 16) {
    p::Capture c;
    c.bounds.include({-1, -1, -1});
    c.bounds.include({1, 1, 1});
    c.resolution = resolution;
    for (const auto &ray : p::rays(c))
      c.radiance.push_back({1 + ray.normal[0], 1 + ray.normal[1], 1 + ray.normal[2], 0});
    return c;
  }
  void cubeCorners() {
    const auto source = capture(8);
    auto data = p::filter({&source, 1}, 64);
    const uint32_t last = p::levelOffset(8, data.header.parameters[2]-1);
    for (uint32_t face = 0; face < 6; ++face)
      data.texels[last+face] = {float(face+1), 0, 0, 0};
    for (float x : {-1.f, 1.f})
      for (float y : {-1.f, 1.f})
        for (float z : {-1.f, 1.f}) {
          const float expected = ((x > 0 ? 1.f : 2.f) + (y > 0 ? 3.f : 4.f) +
                                  (z > 0 ? 5.f : 6.f)) / 3;
          for (uint32_t axis = 0; axis < 3; ++axis) {
            Vec3 ray{x, y, z};
            ray[axis] *= 1.00001f;
            const auto value = p::sample(data, {}, ray, 1);
            require(value && std::abs((*value)[0]-expected) < 1e-4f && (*value)[1] == 0,
                    "All eight cube corners average three faces independently of the winning major axis");
          }
        }
  }
} // namespace
int main() {
  {
    auto first = capture(64), second = first;
    second.position[0] = .25f;
    second.radiance[19] = {12000, 300, .125f, 0};
    const std::array captures{first, second, first};
    const auto serial = p::serialize(p::filter(captures, 64));
    remix::WorkerPool workers(2);
    for (unsigned repeat = 0; repeat < 2; ++repeat)
      require(p::serialize(p::filter(captures, 64, {}, &workers)) == serial,
              "Parallel reflection rows preserve every packed byte, including shared captures");
    std::stop_source stopped;
    stopped.request_stop();
    bool cancelled = false;
    try {
      p::filter(captures, 64, stopped.get_token(), &workers);
    } catch (const std::runtime_error &) {
      cancelled = true;
    }
    require(cancelled, "Parallel reflection filtering respects cancellation");
  }
  testCaptureWork();
  testProbeImageData();
  cubeCorners();
  require(p::imageCapacity(128, 4096, 256, 128ull << 20) == 64 &&
              p::imageCapacity(512, 4096, 256, sizeof(p::Header)) == 64 &&
              p::imageCapacity(512, 3071, 256, sizeof(p::Header)) == 0 &&
              p::imageCapacity(8, 4096, 16, sizeof(p::Header)) == 16 &&
              p::imageCapacity(8, 4096, 256, sizeof(p::Header)-1) == 0,
          "Image capacity preserves captures within independent image and metadata limits");

  for (uint32_t face = 0; face < 6; ++face)
    for (float u : {-.9f, 0.f, .9f})
      for (float v : {-.9f, 0.f, .9f}) {
        const auto projected = p::project(p::direction(face, u, v));
        require(projected.face == face && std::abs(projected.uv[0] - u) < 1e-6f &&
                    std::abs(projected.uv[1] - v) < 1e-6f,
                "Cube face orientation round-trips in all quadrants");
      }
  auto source = capture();
  remix::scene::BakeScene scene;
  scene.hasBounds = true;
  scene.minimum = {-1, -1, -1};
  scene.maximum = {1, 1, 1};
  remix::forward::bake::Grid grid;
  for (uint32_t index = 0; index < 8; ++index) {
    grid.points.push_back(
        {{index & 1u ? .5f : -.5f, index & 2u ? .5f : -.5f, index & 4u ? .5f : -.5f, 0}, {}});
    grid.tasks.push_back(index);
    grid.probes.emplace_back().statistics[3] = 1;
  }
  const auto placed = p::place(scene, grid, 4, 8);
  require(placed.size() == 4, "Playable volumes receive spatially distributed reflection captures");
  for (const auto &probe : placed)
    require(std::abs(probe.position[0]) == .5f && std::abs(probe.position[1]) == .5f &&
                probe.radiance.size() == 6 * 8 * 8,
            "Reflection captures reuse validated irradiance sites and contain six complete faces");
  for (auto &probe : grid.probes)
    probe.statistics[3] = 0;
  require(p::place(scene, grid, 4, 8).empty(),
          "Invalid irradiance sites cannot become reflection capture origins");
  auto data = p::filter({&source, 1}, 256);
  require(data.texels.size() == p::levelOffset(16, 5) &&
              p::serialize(data).size() == sizeof(p::Header) + data.texels.size() * 16,
          "Every mip and face is present in the packed GPU layout");
  const auto center = p::sample(data, {0, 0, 0}, {0, 0, 1}, 0);
  const auto displaced = p::sample(data, {.5f, 0, 0}, {0, 0, 1}, 0);
  require(center && displaced && std::abs((*center)[0] - 1) < .01f && (*displaced)[0] > (*center)[0] + .4f,
          "Box projection moves a reflected wall feature with the receiver");
  auto independent = source;
  independent.influence.include({-2, -2, -2});
  independent.influence.include({2, 2, 2});
  const auto independentData = p::filter({&independent, 1}, 64);
  const auto separateBounds = p::sample(independentData, {.5f, 0, 0}, {0, 0, 1}, 0);
  require(separateBounds && displaced && std::abs((*separateBounds)[0] - (*displaced)[0]) < 1e-5f,
          "Changing influence bounds does not change parallax projection");
  const auto outsideProxy = p::sample(independentData, {1.5f, 0, 0}, {1, 0, 0}, 0);
  require(outsideProxy && (*outsideProxy)[0] > 1.9f,
          "Influence beyond a projection face never reverses an outward reflected ray");
  const auto edgeA = p::sample(data, {0, 0, 0}, {1, 0, .9999f}, 0);
  const auto edgeB = p::sample(data, {0, 0, 0}, {.9999f, 0, 1}, 0);
  require(edgeA && edgeB && std::abs((*edgeA)[0] - (*edgeB)[0]) < .001f,
          "Bilinear taps cross face boundaries without an edge discontinuity");
  const auto rough = p::sample(data, {0, 0, 0}, {0, 0, 1}, 1);
  require(rough && (*rough)[2] < (*center)[2] - .15f && (*rough)[2] > 1,
          "Roughness convolves directional radiance rather than scaling a sharp reflection");
  require(!p::sample(data, {2, 0, 0}, {0, 0, 1}, 0) && !p::sample(data, {0, 0, 0}, {0, 0, 0}, 0),
          "Outside and invalid queries preserve the caller's fallback");
  p::Capture constants[2] = {source, source};
  for (auto &pixel : constants[0].radiance)
    pixel = {4, 0, 0, 0};
  for (auto &pixel : constants[1].radiance)
    pixel = {0, 2, 0, 0};
  const auto blended = p::filter(constants, 64);
  for (float roughness : {0.f, .3f, 1.f}) {
    const auto value = p::sample(blended, {0, 0, 0}, {1, 0, 0}, roughness);
    require(value && std::abs((*value)[0] - 2) < 1e-5f && std::abs((*value)[1] - 1) < 1e-5f,
            "Overlapping probes blend once and preserve HDR energy through every roughness mip");
  }
  // Rotate a narrow box 90 degrees about a translated center. Its capture
  // remains off-center in world space; the captured directions do not rotate.
  auto oriented = source;
  oriented.bounds.minimum = {-2, -.5f, -1};
  oriented.bounds.maximum = {2, .5f, 1};
  oriented.worldToLocal = {{{0, 1, 0, -20}, {-1, 0, 0, 10}, {0, 0, 1, -30}}};
  oriented.position = {10, 20.25f, 30};
  const auto orientedData = p::filter({&oriented, 1}, 64);
  require(p::sample(orientedData, {10, 21.5f, 30}, {1, 0, 0}, 0).has_value() &&
              !p::sample(orientedData, {11, 20, 30}, {1, 0, 0}, 0),
          "Rotated influence uses local bounds instead of a world AABB");
  // Analytic +X exit of the rotated box is x=10.5. Reflection direction from
  // off-center capture to (10.5,21.5,30) stays in the world cubemap frame.
  const auto actual = p::sample(orientedData, {10, 21.5f, 30}, {1, 0, 0}, 0);
  const auto expected = p::sample(data, {0, 0, 0}, {.5f, 1.25f, 0}, 0);
  require(actual && expected && std::abs((*actual)[0]-(*expected)[0]) < 1e-5f,
          "Oriented parallax samples a world-space hit direction from the independent capture origin");
  auto invalidFrame = oriented;
  invalidFrame.worldToLocal[0][1] = 2;
  bool rejectedFrame = false;
  try { p::filter({&invalidFrame, 1}, 64); } catch (const std::invalid_argument &) { rejectedFrame = true; }
  require(rejectedFrame, "Projection transforms reject scaling instead of distorting ray distances");
  constants[1].priority = 4;
  constants[1].influence.minimum = {-.25f, -.25f, -.25f};
  constants[1].influence.maximum = {.25f, .25f, .25f};
  constants[1].influence.valid = true;
  const auto prioritized = p::filter(constants, 64);
  const auto priorityInside = p::sample(prioritized, {0, 0, 0}, {1, 0, 0}, .4f);
  const auto priorityOutside = p::sample(prioritized, {.5f, 0, 0}, {1, 0, 0}, .4f);
  require(priorityInside && (*priorityInside)[0] < 1e-5f && std::abs((*priorityInside)[1]-2) < 1e-5f &&
              priorityOutside && std::abs((*priorityOutside)[0]-4) < 1e-5f && (*priorityOutside)[1] < 1e-5f,
          "Highest core priority wins; leaving its influence restores the lower-priority capture");
  std::swap(constants[0], constants[1]);
  const auto reversed = p::sample(p::filter(constants, 64), {0, 0, 0}, {1, 0, 0}, .4f);
  require(reversed && (*reversed) == (*priorityInside), "Overlap priority is independent of capture ordering");
  std::swap(constants[0], constants[1]);
  {
    std::array shared{constants[0], constants[1]};
    shared[1].blendDistance = .05f;
    shared[1].influence.minimum[2] = -1;
    const auto blended = p::filter(shared, 64);
    for (float roughness : {0.f, .4f, 1.f})
      for (float z : {0.f, -1.f})
        for (float t : {0.f, .25f, .5f, .75f, 1.f}) {
          const float alpha = t * t * (3 - 2 * t);
          const auto value = p::sample(blended, {.25f - .05f*t, 0, z}, {1, 0, 0}, roughness);
          require(value && std::abs((*value)[0] - 4*(1-alpha)) < 5e-5f &&
                      std::abs((*value)[1] - 2*alpha) < 5e-5f,
                  "Priority feathering follows analytic layer opacity, including a shared floor");
          if (roughness == 0 && z == -1)
            std::printf("Priority shared-floor blend t %.2f: %.7f %.7f %.7f\n", t,
                        (*value)[0], (*value)[1], (*value)[2]);
        }
    auto top = shared[1];
    top.priority = 9;
    top.influence.minimum[0] = -.2375f;
    top.influence.maximum[0] = .2375f;
    for (auto &pixel : top.radiance) pixel = {0, 0, 8, 0};
    const std::array layers{shared[0], shared[1], top};
    const std::array reversedLayers{top, shared[1], shared[0]};
    const auto layered = p::sample(p::filter(layers, 64), {.225f, 0, -1}, {1, 0, 0}, .4f);
    const auto reordered = p::sample(p::filter(reversedLayers, 64), {.225f, 0, -1}, {1, 0, 0}, .4f);
    require(layered && reordered && std::abs((*layered)[0]-1.6875f) < 5e-5f &&
                std::abs((*layered)[1]-.84375f) < 5e-5f && std::abs((*layered)[2]-1.25f) < 5e-5f &&
                *layered == *reordered,
            "Three priority layers composite front to back independently of capture order");
    auto bright = p::filter(layers, 64);
    for (auto &pixel : bright.texels) pixel = {65000, 65000, 65000, 0};
    const auto hdr = p::sample(bright, {.225f, 0, -1}, {1, 0, 0}, .4f);
    require(hdr && std::all_of(hdr->begin(), hdr->end(), [](float value) {
              return std::isfinite(value) && std::abs(value-65000) < .02f;
            }), "Priority layers preserve finite HDR energy on a shared boundary");
  }
  constants[1].priority = 0;
  constants[1].influence = {};
  // Both volumes touch the same floor. Box-edge distance is zero for every
  // floor receiver, so it cannot distinguish the nearby and distant captures.
  constants[0].position = {-.5f, 0, 0};
  constants[1].position = {.5f, 0, 0};
  const auto floorBlend = p::filter(constants, 64);
  for (float roughness : {0.f, .3f, 1.f}) {
    const auto left = p::sample(floorBlend, {-.5f, 0, -1}, {0, 0, 1}, roughness);
    const auto right = p::sample(floorBlend, {.5f, 0, -1}, {0, 0, 1}, roughness);
    require(left && right && (*left)[0] > (*right)[0] + .5f && (*right)[1] > (*left)[1] + .25f,
            "Shared floor boundaries retain spatial blending between nearby captures");
    require(std::abs((*left)[0] / 4 + (*left)[1] / 2 - 1) < 1e-5f,
            "Boundary blending preserves HDR energy");
    float previous = 5;
    for (int step = 0; step <= 20; ++step) {
      const float x = -.5f + step * .05f;
      const auto value = p::sample(floorBlend, {x, 0, -1}, {0, 0, 1}, roughness);
      require(value && std::isfinite((*value)[0]) && (*value)[0] <= previous &&
                  previous - (*value)[0] < .15f + (step == 0 ? 5.f : 0.f),
              "Capture preference changes continuously along a shared floor");
      previous = (*value)[0];
    }
    auto scaled = floorBlend;
    for (auto &record : scaled.header.probes) {
      for (auto *coordinate : {&record.minimum, &record.maximum, &record.influenceMinimum,
                               &record.influenceMaximum, &record.position})
        for (uint32_t axis = 0; axis < 3; ++axis)
          (*coordinate)[axis] *= 40;
      record.maximum[3] /= 40;
      record.influenceMinimum[3] /= 40;
      record.influenceMaximum[3] /= 40;
    }
    const auto scaledLeft = p::sample(scaled, {-20, 0, -40}, {0, 0, 1}, roughness);
    require(scaledLeft && std::abs((*scaledLeft)[0] - (*left)[0]) < 1e-5f,
            "Reflection blend weights are independent of world coordinate units");
    std::printf("Floor roughness %.1f: left %.4f/%.4f, right %.4f/%.4f; continuous and scale invariant\n",
                roughness, (*left)[0], (*left)[1], (*right)[0], (*right)[1]);
  }
  auto supported = source;
  std::array<p::Capture, 2> overlapCaptures{constants[0], constants[1]};
  overlapCaptures[0].bounds = {};
  overlapCaptures[0].bounds.include({-2, -1, -1});
  overlapCaptures[0].bounds.include({.5f, 1, 1});
  overlapCaptures[0].position = {-1, 0, 0};
  overlapCaptures[1].bounds = {};
  overlapCaptures[1].bounds.include({-.5f, -1, -1});
  overlapCaptures[1].bounds.include({2, 1, 1});
  overlapCaptures[1].position = {1, 0, 0};
  for (auto &capture : overlapCaptures) {
    capture.influence = {};
    capture.priority = 0;
    capture.blendDistance = .5f;
  }
  const auto overlapData = p::filter(overlapCaptures, 64);
  const auto floorEntry = p::sample(overlapData, {-.4f, 0, -1}, {0, 0, 1}, 0);
  require(
      floorEntry && (*floorEntry)[1] < .15f,
      "A common floor does not erase the entering capture's side feather band");
  float previousRed = 5;
  for (int step = 0; step <= 200; ++step) {
    const float x = -.55f + step * .0055f;
    const auto value = p::sample(overlapData, {x, 0, 0}, {0, 0, 1}, .3f);
    require(value && (*value)[0] <= previousRed + 1e-5f &&
                std::abs((*value)[0] - previousRed) <
                    .1f + (step == 0 ? 5.f : 0.f),
            "Feather bands blend overlapping captures continuously at entry, "
            "core and exit");
    require(std::abs((*value)[0] / 4 + (*value)[1] / 2 - 1) < 1e-5f,
            "Overlapping feather weights preserve linear HDR energy");
    previousRed = (*value)[0];
  }
  auto legacyBlend = overlapData;
  for (auto &record : legacyBlend.header.probes)
    record.maximum[3] = record.influenceMinimum[3] =
        record.influenceMaximum[3] = 0;
  require(
      p::sample(legacyBlend, {0, 0, 0}, {0, 0, 1}, 0).has_value(),
      "Legacy cache metadata derives an edge width without retracing lighting");
  const auto depthRays = p::rays(supported);
  for (size_t i = 0; i < supported.radiance.size(); ++i) {
    const auto &d = depthRays[i].normal;
    supported.radiance[i][3] = 1 / std::max({std::abs(d[0]), std::abs(d[1]), std::abs(d[2])});
  }
  require(p::projectionConfidence(supported) == 1 && p::projectionConfidence(source) == 1,
          "An exact room box and unknown legacy depth keep full reflection preference");
  auto interiorProps = supported;
  for (auto &pixel : interiorProps.radiance)
    pixel[3] *= .3f;
  require(p::projectionConfidence(interiorProps) == 1,
          "Interior props do not disqualify an otherwise valid room projection");
  auto doorway = supported;
  for (auto &pixel : doorway.radiance)
    pixel[3] *= 4;
  require(std::abs(p::projectionConfidence(doorway) - .05f) < 1e-6f,
          "A box far short of the captured geometry receives bounded low preference");
  auto opening = supported;
  for (size_t i = 0; i < 16 * 16; ++i)
    opening.radiance[i][3] *= 4;
  require(std::abs(p::projectionConfidence(opening) - 5.f / 6) < 1e-5f,
          "One open cube face lowers preference in proportion to its solid angle");
  auto scaledOpening = opening;
  for (auto *coordinate : {&scaledOpening.bounds.minimum, &scaledOpening.bounds.maximum,
                           &scaledOpening.position})
    for (auto &axis : *coordinate)
      axis *= 40;
  for (auto &pixel : scaledOpening.radiance)
    pixel[3] *= 40;
  require(std::abs(p::projectionConfidence(scaledOpening) - p::projectionConfidence(opening)) < 1e-6f,
          "Projection confidence is independent of world coordinate units");
  p::Capture reliableBlend[] = {supported, doorway};
  for (auto &pixel : reliableBlend[0].radiance)
    pixel = {4, 0, 0, pixel[3]};
  for (auto &pixel : reliableBlend[1].radiance)
    pixel = {0, 2, 0, pixel[3]};
  const auto preferred = p::filter(reliableBlend, 16);
  const auto sole = p::filter({&reliableBlend[1], 1}, 16);
  for (float roughness : {0.f, .3f, 1.f}) {
    const auto value = p::sample(preferred, {0, 0, -1}, {0, 0, 1}, roughness);
    const auto alone = p::sample(sole, {0, 0, -1}, {0, 0, 1}, roughness);
    require(value && (*value)[0] > 3.5f && (*value)[1] < .25f &&
                std::abs((*value)[0] / 4 + (*value)[1] / 2 - 1) < 1e-5f,
            "Overlapping captures prefer supported room geometry without losing HDR energy");
    require(alone && std::abs((*alone)[1] - 2) < 1e-5f,
            "A sole poorly approximated capture retains its full radiance at every roughness");
  }
  auto legacy = preferred;
  for (auto &record : legacy.header.probes)
    record.position[3] = 0;
  const auto neutral = p::sample(legacy, {0, 0, -1}, {0, 0, 1}, .3f);
  require(neutral && std::abs((*neutral)[0] - 2) < 1e-5f && std::abs((*neutral)[1] - 1) < 1e-5f,
          "Legacy zero metadata preserves the original normalized blend");
  std::printf("Projection preference: supported %.3f, doorway %.3f, opening %.3f; sole probe energy retained\n",
              p::projectionConfidence(supported), p::projectionConfidence(doorway),
              p::projectionConfidence(opening));
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try {
    p::filter({&source, 1}, 128, stop.get_token());
  } catch (const std::runtime_error &) {
    cancelled = true;
  }
  require(cancelled, "Superseded reflection filtering stops before preparing a replacement asset");
  source.radiance.pop_back();
  bool refused = false;
  try {
    p::filter({&source, 1});
  } catch (const std::invalid_argument &) {
    refused = true;
  }
  require(refused, "Incomplete cubemaps are refused before publication");
  std::puts("Cube orientation, parallax, seam filtering, GGX roughness and HDR volume blending passed");
}
