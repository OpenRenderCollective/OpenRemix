#include "forward/bake/adaptive_grid.h"
#include "forward/bake/receiver_support.h"
#include "forward/lightmap/layout.h"
#include "forward/half.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {
  using namespace remix;
  using namespace remix::forward;
  void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
  }
  bake::Grid base() {
    bake::Grid grid;
    grid.header.originSpacing = {0, 0, 0, 4};
    grid.header.dimensions = {5, 3, 3, 45};
    grid.header.settings = {.08f, .016f, 2, 0};
    for (uint32_t z = 0; z < 3; ++z)
      for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 5; ++x) {
          grid.points.push_back({{float(x * 4), float(y * 4), float(z * 4), 0}, {}});
          grid.tasks.push_back(uint32_t(grid.probes.size()));
          grid.lookup.push_back(uint32_t(grid.probes.size()) + 1);
          bake::Result result;
          result.statistics[3] = 1;
          result.irradiance[0][0] = x >= 3 ? 10 : 1;
          for (auto &moment : result.moments) moment = {100, 10000, 0, 1};
          grid.probes.push_back(result);
        }
    grid.origins = bake::directoryOrigins(grid);
    return grid;
  }
  void narrowPlacementTests() {
    scene::BakeScene scene;
    scene.sceneScale = 1;
    scene.hasBounds = true;
    scene.minimum = {0, 0, 0};
    scene.maximum = {4, 4, 4};
    auto partition = std::make_shared<scene::VisibilityPartition>();
    partition->closed = true;
    partition->clusterCount = 1;
    partition->nodes = {{{1,0,0,1}, {1,-2}, {}}, {{1,0,0,3}, {-2,-1}, {}}};
    partition->leafClusters = {0, -1};
    scene.partition = partition;
    lightmap::Layout layout;
    layout.points = {{{2,2,2,0}, {0,0,1,0}}};
    bake::Config config;
    config.spacing = 1;
    config.maxProbes = 512;
    config.relocateProbes = false;
    auto coarse = config;
    coarse.spacing *= 8;
    coarse.maxProbes /= 2;
    bool empty = false;
    try { bake::place(scene, coarse); }
    catch (const bake::EmptyProbePlacement &) { empty = true; }
    require(empty, "Coarse fixture misses a narrow open slab that the requested lattice can sample");
    bake::Refinement refinement;
    const auto fallback = bake::placeRefinementBase(scene, config, layout, refinement);
    const auto ordinary = bake::placeSparse(scene, config, layout);
    require(refinement.factor == 0 && refinement.recordBudget == 0 && !fallback.tasks.empty() &&
                fallback.header.originSpacing == ordinary.header.originSpacing &&
                fallback.lookup == ordinary.lookup && fallback.tasks == ordinary.tasks,
            "An empty coarse seed falls back to verified full-budget ordinary placement");
    partition->leafClusters[0] = -1;
    empty = false;
    try { bake::placeRefinementBase(scene, config, layout, refinement); }
    catch (const bake::EmptyProbePlacement &) { empty = true; }
    require(empty, "A partition with no usable leaves still fails rather than publishing empty lighting");
    partition->leafClusters[0] = 0;
    std::stop_source stop;
    stop.request_stop();
    bool cancelled = false;
    try { bake::placeRefinementBase(scene, config, layout, refinement, stop.get_token()); }
    catch (const std::runtime_error &) { cancelled = true; }
    require(cancelled, "An adaptive fallback must not swallow cancellation");

    // Here budget fitting places a valid 1.5-unit lattice, but a denser trial
    // at the requested 1-unit spacing misses the thin leaf entirely.
    scene.maximum = {1.5f, 1, 1};
    partition->nodes[0].plane[3] = 1.45f;
    partition->nodes[1].plane[3] = 1.55f;
    config.maxProbes = 64;
    const auto fitting = bake::place(scene, config);
    auto dense = config;
    dense.maxProbes = 512;
    empty = false;
    try { bake::place(scene, dense); }
    catch (const bake::EmptyProbePlacement &) { empty = true; }
    require(empty && fitting.header.originSpacing[3] == 1.5f,
            "Sparse search fixture has a valid fitting lattice and an empty denser trial");
    const auto sparse = bake::placeSparse(scene, config, layout);
    require(sparse.header.originSpacing == fitting.header.originSpacing &&
                sparse.tasks.size() == fitting.tasks.size(),
            "An empty density-search trial preserves the last verified fitting lattice");
  }
}
void refinementTests() {
  narrowPlacementTests();
  {
    bake::GridHeader header;
    header.originSpacing = {0, 0, 0, 1};
    header.dimensions = {7, 7, 7, 343};
    lightmap::Layout receivers;
    receivers.points = {{{3.25f, 3.25f, 3.25f, 0}, {}}, {{.25f, .25f, .25f, 0}, {}}};
    receivers.receiverSamples = {1, 0};
    auto occupied = [](const auto &mask) { return std::count(mask.begin(), mask.end(), uint8_t(1)); };
    const auto exact = bake::receiverSupport(header, receivers, 0);
    require(occupied(exact) == 8 && !exact[0] && exact[(3 * 7 + 3) * 7 + 3],
            "True receivers mark all eight interpolation corners while padding donors spend no support");
    const auto halo = bake::receiverSupport(header, receivers, .1f);
    require(occupied(halo) == 64, "A fractional world halo rounds outward to complete neighbouring cells");
    require(occupied(bake::receiverSupport(header, receivers, 1.1f)) == 216 &&
                occupied(bake::receiverSupport(header, receivers, 1e20f)) == 343,
            "Separable support dilation clips to grid bounds even for extreme halo sizes");
    receivers.points[0].position = {6, 6, 6, 0};
    require(occupied(bake::receiverSupport(header, receivers, 0)) == 8,
            "A receiver on the maximum boundary retains the final complete interpolation cell");
    receivers.points[0].position = {6.25f, 3, 3, 0};
    require(occupied(bake::receiverSupport(header, receivers, 0)) == 0 &&
                occupied(bake::receiverSupport(header, receivers, .5f)) > 0,
            "Nearby receivers outside a patch contribute their halo across its boundary");
    std::stop_source stop;
    stop.request_stop();
    bool cancelled = false;
    try { bake::receiverSupport(header, receivers, 0, stop.get_token()); }
    catch (const std::runtime_error &) { cancelled = true; }
    require(cancelled, "Support analysis respects superseded work before scanning receivers");
    header.originSpacing[3] = 4;
    header.dimensions = {4, 2, 2, 16};
    receivers.points = {{{3.9f, 2, 2, 0}, {}}, {{4.1f, 2, 2, 0}, {}}, {{10, 2, 2, 0}, {}}};
    std::vector<std::vector<uint32_t>> bins(16);
    bins[0] = {0}; bins[1] = {1}; bins[2] = {2};
    require(bake::nearbyReceivers(header, receivers, bins, 0, 1) == std::vector<uint32_t>{0, 1} &&
                bake::nearbyReceivers(header, receivers, bins, 1, 1) == std::vector<uint32_t>{0, 1},
            "Adjacent patch neighbourhoods share border receivers in deterministic source order");
  }
  scene::BakeScene scene;
  scene.sceneScale = 1;
  scene.hasBounds = true;
  scene.minimum = {0, 0, 0};
  scene.maximum = {20, 20, 20};
  auto partition = std::make_shared<scene::VisibilityPartition>();
  partition->closed = true;
  partition->clusterCount = 1;
  partition->nodes.push_back({{1, 0, 0, -1}, {-1, -2}, {0, 0}});
  partition->leafClusters = {0, -1};
  scene.partition = partition;
  lightmap::Layout layout;
  layout.points = {{{2, 2, 2, 0}, {0, 0, 1, 0}}, {{10, 2, 2, 0}, {0, 0, 1, 0}}};
  const auto original = base();
  auto grid = original;
  // Cover both cells before spending the remainder on one denser patch.
  const bake::Refinement policy{4, 45 + (125 - 8) + (27 - 8), false};
  require(bake::refineGrid(grid, scene, layout, policy) == 2,
          "A bounded refinement budget covers both receiver regions before upgrading one");
  require(grid.probes.size() == policy.recordBudget && grid.tasks.size() == grid.probes.size(),
          "Shared coarse corners consume neither extra records nor duplicate tracing tasks");
  require(bake::patchFactor(grid.lookup[49], 4) == 2 && bake::patchFactor(grid.lookup[51], 4) == 4,
          "Measured lighting variation prioritizes the discontinuous cell over an equally occupied flat cell");
  require(std::memcmp(grid.probes.data(), original.probes.data(), original.probes.size() * sizeof(bake::Result)) == 0,
          "Refinement preserves every already traced coarse result");
  for (uint32_t i = 0; i < grid.tasks.size(); ++i)
    require(grid.tasks[i] == i, "Fine work is appended once after the completed coarse prefix");
  auto repeated = original;
  require(bake::refineGrid(repeated, scene, layout, policy) == 2 && repeated.lookup == grid.lookup &&
              repeated.origins == grid.origins && repeated.tasks == grid.tasks,
          "Repeated planning is deterministic, including budget exhaustion and shared origins");
  auto localOnly = layout;
  localOnly.points.resize(1);
  auto supportedGrid = original;
  require(bake::refineGrid(supportedGrid, scene, localOnly, {8, 45 + 306, false}) == 1 &&
              supportedGrid.probes.size() == 45 + 306,
          "Fine patches spend records on complete receiver-supported cells instead of their entire air volume");
  const auto block = bake::patchOffset(supportedGrid.lookup[49]);
  require(supportedGrid.lookup[block] != 0 && supportedGrid.lookup[block + 1] == 0 &&
              supportedGrid.lookup[block + (4 * 9 + 4) * 9 + 4] != 0,
          "Upgrades retain outer coarse support and leave unsupported fine nodes explicitly empty");
  auto exhausted = original;
  require(bake::refineGrid(exhausted, scene, layout, {4, 45, false}) == 0 &&
              exhausted.lookup == original.lookup && exhausted.header.settings == original.header.settings,
          "An exhausted budget retains the complete existing field without malformed adaptive metadata");
  auto donors = layout;
  donors.receiverSamples = {1, 0};
  auto donorGrid = original;
  require(bake::refineGrid(donorGrid, scene, donors, policy) == 1 && donorGrid.lookup[49] != 0 &&
              donorGrid.lookup[51] == 0,
          "Padding donor samples do not spend refinement budget on unoccupied cells");
  auto measured = layout;
  measured.points.resize(33, layout.points[1]);
  std::vector<bake::Result> reference(measured.points.size());
  reference[0].irradiance[0][0] = 10 / .2820947918f;
  const auto accurate = bake::sample(original, {10, 2, 2}, {0, 0, 1});
  require(accurate.has_value(), "Measured-priority fixture has supported coarse lighting");
  for (uint32_t i = 1; i < reference.size(); ++i)
    reference[i].irradiance[0][0] = (*accurate)[0] / .2820947918f;
  auto measuredGrid = original;
  require(bake::refineGrid(measuredGrid, scene, measured, policy, reference) == 2 &&
              bake::patchFactor(measuredGrid.lookup[49], 4) == 4 &&
              bake::patchFactor(measuredGrid.lookup[51], 4) == 2,
          "Actual lightmap disagreement takes priority over coarse variance and a larger receiver count");
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try { bake::refineGrid(exhausted, scene, layout, policy, {}, stop.get_token()); }
  catch (const std::runtime_error &) { cancelled = true; }
  require(cancelled && exhausted.lookup == original.lookup,
          "Superseded planning stops before changing the retained field");
  bake::Config config;
  config.maxProbes = 512;
  config.spacing = 1;
  bake::Refinement selected;
  const auto coarse = bake::placeRefinementBase(scene, config, layout, selected);
  require(selected.factor >= 2 && coarse.probes.size() <= 256 && selected.recordBudget == 512,
          "Eligible closed partitions reserve a bounded second-pass record budget");
  scene.partition.reset();
  const auto dense = bake::placeRefinementBase(scene, config, layout, selected);
  const auto plain = bake::place(scene, config);
  require(selected.factor == 0 && dense.header.dimensions == plain.header.dimensions &&
              dense.header.originSpacing == plain.header.originSpacing,
          "Unpartitioned hosts retain their existing complete placement policy");
  {
    auto covered = grid;
    for (auto &probe : covered.probes) {
      probe.statistics[3] = 1;
      for (auto &moment : probe.moments) moment = {100, 10000, 0, 1};
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        for (uint32_t c = 0; c < 3; ++c) probe.irradiance[coefficient][c] = coefficient == 0 ? 4.f : 0.f;
    }
    const Vec3 receiver{1, 2, 2}, normal{0, 0, 1};
    const auto legacy = bake::probeStencil(covered, receiver, normal);
    float legacyCoarse = 0;
    for (uint32_t i = 0; i < 8; ++i) legacyCoarse += legacy.weights[i];
    covered.depth.maximumDistance = 100;
    covered.depth.records.resize(covered.probes.size());
    const uint32_t weak = uint32_t(half::encode(.02f)) | uint32_t(half::encode(.0005f)) << 16;
    const uint32_t open = uint32_t(half::encode(1.f)) | uint32_t(half::encode(1.f)) << 16;
    for (uint32_t record = 0; record < covered.probes.size(); ++record)
      covered.depth.records[record].fill(record < original.probes.size() ? weak : open);
    const auto directional = bake::probeStencil(covered, receiver, normal);
    float coarseShare = 0, total = 0;
    for (uint32_t i = 0; i < directional.count; ++i) {
      total += directional.weights[i];
      if (i < 8) coarseShare += directional.weights[i];
    }
    require(legacyCoarse > .1f && coarseShare < legacyCoarse * .5f && std::abs(total - 1) < 1e-5f,
            "Weak blocked coarse support cannot regain its full blend share beside confident fine probes");
    const auto constant = bake::sample(covered, receiver, normal);
    require(constant && std::abs((*constant)[0] - 4 * .2820947918f) < 1e-5f,
            "Confidence-aware adaptive mixing preserves constant irradiance energy");
    for (auto &probe : covered.probes)
      for (auto &coefficient : probe.irradiance)
        for (uint32_t c = 0; c < 3; ++c) coefficient[c] = 0;
    const auto black = bake::sample(covered, receiver, normal);
    require(black && *black == Vec3{}, "Directional visibility retains valid black coverage");
  }
}
