#include "forward/probes/data.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace remix::forward;
namespace p = remix::forward::probes;
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
} // namespace
int main() {
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
    for (auto &record : scaled.header.probes)
      for (auto *coordinate : {&record.minimum, &record.maximum, &record.influenceMinimum,
                               &record.influenceMaximum, &record.position})
        for (uint32_t axis = 0; axis < 3; ++axis)
          (*coordinate)[axis] *= 40;
    const auto scaledLeft = p::sample(scaled, {-20, 0, -40}, {0, 0, 1}, roughness);
    require(scaledLeft && std::abs((*scaledLeft)[0] - (*left)[0]) < 1e-5f,
            "Reflection blend weights are independent of world coordinate units");
    std::printf("Floor roughness %.1f: left %.4f/%.4f, right %.4f/%.4f; continuous and scale invariant\n",
                roughness, (*left)[0], (*left)[1], (*right)[0], (*right)[1]);
  }
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
