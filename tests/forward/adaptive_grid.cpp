#include "forward/bake/grid.h"
#include "forward/bake/spherical_harmonics.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace remix::forward;
void refinementTests();
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  bake::Grid field() {
    bake::Grid grid;
    grid.header.originSpacing = {0, 0, 0, 4};
    grid.header.dimensions = {3, 2, 2, 39};
    grid.header.settings = {.08f, .016f, 4, 0};
    grid.probes.resize(39);
    grid.lookup.resize(56);
    for (uint32_t node = 0; node < 12; ++node) grid.lookup[node] = node + 1;
    grid.lookup[12] = bake::adaptiveDirectoryMagic;
    grid.lookup[13] = 2;
    grid.lookup[14] = uint32_t(grid.lookup.size());
    grid.lookup[15] = 12;
    grid.lookup[16] = 28; // first coarse cell has a 3^3 fine-node block
    for (uint32_t node = 0; node < 27; ++node) grid.lookup[28 + node] = 13 + node;
    for (uint32_t i = 0; i < grid.probes.size(); ++i) {
      auto &probe = grid.probes[i];
      probe.statistics[3] = 1;
      probe.irradiance[0] = {(i < 12 ? 2.f : 4.f) / .2820947918f, 0, 0, 0};
      probe.irradiance[7][3] = 1;
      probe.irradiance[8][3] = i < 12 ? .25f : .75f;
      for (auto &moment : probe.moments) moment = {100, 10000, 0, 1};
    }
    grid.origins = bake::directoryOrigins(grid);
    return grid;
  }
  void rejects(auto edit) {
    auto grid = field();
    edit(grid);
    bool refused = false;
    try { bake::directoryOrigins(grid); }
    catch (const std::invalid_argument &) { refused = true; }
    require(refused, "Malformed refinement metadata is rejected before sampling");
  }
}
int main() {
  refinementTests();
  auto grid = field();
  const auto published = bake::publicationHeader(grid.header, grid.lookup.size());
  const auto addressing = std::bit_cast<uint32_t>(published.settings[2]);
  require((addressing & 7u) == 4 && (addressing >> 3) == 14 && grid.header.settings[2] == 4 &&
              published.originSpacing == grid.header.originSpacing && published.dimensions == grid.header.dimensions,
          "GPU publication supplies directory addressing directly without changing CPU/cache metadata");
  require(grid.origins.size() == 39 && grid.origins[12].spacing == 2 &&
              grid.origins[38].position == Vec3{4, 4, 4},
          "Fine physical records have canonical world origins and local spacing");
  const Vec3 normal{0, 0, 1};
  for (const auto [x, expected] : {std::pair{0.f, 2.f}, {1.f, 3.f}, {2.f, 4.f},
                                 {3.f, 3.f}, {4.f, 2.f}, {5.f, 2.f}}) {
    const auto value = bake::sample(grid, {x, 2, 2}, normal);
    require(value && std::abs((*value)[0] - expected) < 2e-5f,
            "Adaptive reconstruction blends normalized coarse and fine fields at the expected distance");
    const auto sun = bake::sampleSunVisibility(grid, {x, 2, 2}, normal);
    require(sun && std::abs(*sun - (.25f + (expected - 2) * .25f)) < 2e-5f,
            "Static sunlight visibility follows the same smooth transition without adding irradiance");
  }
  const auto left = bake::sample(grid, {4 - 1e-4f, 2, 2}, normal);
  const auto right = bake::sample(grid, {4 + 1e-4f, 2, 2}, normal);
  require(left && right && std::abs((*left)[0] - (*right)[0]) < 1e-6f,
          "The coarse/fine boundary has continuous value across adjacent cells");
  for (auto &probe : grid.probes) {
    probe.irradiance[0][0] = 3.f / .2820947918f;
    probe.irradiance[8][3] = .5f;
  }
  for (float x = .01f; x < 7.99f; x += .07f) {
    const auto value = bake::sample(grid, {x, 2, 2}, normal);
    require(value && std::abs((*value)[0] - 3) < 2e-5f,
            "A constant field conserves energy through every blend weight");
  }
  {
    auto confidence = field();
    for (uint32_t record = 0; record < confidence.probes.size(); ++record)
      for (auto &moment : confidence.probes[record].moments)
        moment = record < 12 ? Vec4{2, 5, 0, 1} : Vec4{100, 10000, 0, 1};
    const auto inside = bake::sample(confidence, {4 - 1e-4f, 2, 2}, normal);
    const auto outside = bake::sample(confidence, {4 + 1e-4f, 2, 2}, normal);
    require(inside && outside && std::abs((*inside)[0] - (*outside)[0]) < 1e-5f,
            "Suppressing weak coarse support retains the coarse/fine boundary limit");
  }
  for (uint32_t i = 12; i < grid.probes.size(); ++i) grid.probes[i].statistics[3] = 0;
  require(bake::sample(grid, {2, 2, 2}, normal).has_value(),
          "Unsupported fine records retain coarse coverage");
  require(!bake::sample(grid, {-10, 2, 2}, normal), "Unknown global regions retain fallback lighting");
  auto shared = field();
  shared.lookup[54] = 11; // (4,4,4) is both a coarse corner and a fine node.
  shared.probes.pop_back();
  --shared.header.dimensions[3];
  shared.origins = bake::directoryOrigins(shared);
  require(shared.origins.size() == 38 && shared.origins[10].position == Vec3{4, 4, 4} &&
              shared.origins[10].spacing == 2 && shared.origins[10].relocationSpacing == 4,
          "Coincident coarse and fine nodes share one physical record with the finest spacing");
  require(grid.origins[25].relocationSpacing == 2,
          "A genuinely fine midpoint retains the finer relocation bound");
  auto mixed = field();
  mixed.header.dimensions[3] = 164;
  mixed.lookup.resize(184);
  mixed.lookup[13] = 4;
  mixed.lookup[14] = 184;
  mixed.lookup[16] = bake::patchReference(28, 2);
  mixed.lookup[17] = bake::patchReference(56, 4);
  mixed.probes.resize(164, mixed.probes[12]);
  for (uint32_t node = 0; node < 125; ++node) {
    mixed.lookup[56 + node] = 40 + node;
    mixed.probes[39 + node].irradiance[0][0] = 8.f / .2820947918f;
  }
  mixed.origins = bake::directoryOrigins(mixed);
  require(mixed.origins[12].spacing == 2 && mixed.origins[39].spacing == 1 &&
              mixed.origins.back().position == Vec3{8, 4, 4},
          "Adjacent patches decode independent subdivision levels and canonical spacing");
  for (const auto [x, expected] : {std::pair{2.f, 4.f}, {4.f, 2.f}, {6.f, 8.f}}) {
    const auto value = bake::sample(mixed, {x, 2, 2}, normal);
    require(value && std::abs((*value)[0] - expected) < 2e-5f,
            "Per-region levels reconstruct their own fields and fade to coarse lighting at a shared edge");
  }
  auto stitched = field();
  stitched.header.dimensions[3] = 57;
  stitched.probes.resize(57, stitched.probes[12]);
  stitched.lookup.resize(84);
  stitched.lookup[14] = 84;
  stitched.lookup[16] = bake::patchReference(28, 2);
  stitched.lookup[17] = bake::patchReference(56, 2);
  uint32_t next = 40;
  for (uint32_t z = 0; z < 3; ++z)
    for (uint32_t y = 0; y < 3; ++y)
      for (uint32_t x = 0; x < 3; ++x)
        stitched.lookup[56 + (z * 3 + y) * 3 + x] = x ? next++ : 13 + (z * 3 + y) * 3 + 2;
  const auto faces = bake::sharedPatchFaces(stitched);
  require(faces[0] == 2 && faces[1] == 1,
          "Only identical shared physical nodes authorize reciprocal no-fade faces");
  for (uint32_t cell = 0; cell < faces.size(); ++cell) stitched.lookup[16 + cell] |= uint32_t(faces[cell]) << 26;
  stitched.origins = bake::directoryOrigins(stitched);
  for (const Vec3 n : {normal, Vec3{1, 0, 0}, Vec3{-1, 0, 0}})
    for (float x : {3.9999f, 4.f, 4.0001f}) {
      const auto value = bake::sample(stitched, {x, 2, 2}, n);
      require(value && std::abs((*value)[0] - 4) < 2e-5f,
              "A shared fine field stays continuous through its boundary, including normal-bias crossings");
    }
  auto forged = stitched;
  forged.lookup[17] &= ~(1u << 26);
  bool refused = false;
  try { bake::directoryOrigins(forged); } catch (const std::invalid_argument &) { refused = true; }
  require(refused, "One-sided stitching is rejected before sampling or cache publication");
  rejects([](auto &g) { g.lookup[12] = 0; });
  rejects([](auto &g) { g.lookup[13] = 3; });
  rejects([](auto &g) { g.lookup[14]--; });
  rejects([](auto &g) { g.lookup[16] = 1; });
  rejects([](auto &g) { g.lookup[17] = 28; });
  rejects([](auto &g) { g.lookup[28] = 1; });
  rejects([](auto &g) { g.lookup[28] = 40; });
  rejects([](auto &g) { g.lookup[29] = g.lookup[28]; });
  rejects([](auto &g) { g.lookup[55] = 1; });
  std::puts("Adaptive directory ownership, spacing, energy, boundaries and visibility passed");
}
