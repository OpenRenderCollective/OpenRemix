#include "forward/bake/grid.h"
#include "forward/lightmap/layout.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace remix;
using namespace remix::forward;
namespace bake = remix::forward::bake;
namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
} // namespace
int main() {
  scene::BakeScene scene;
  scene.sceneScale = .0254f;
  scene.hasBounds = true;
  scene.minimum = {-512, -256, -128};
  scene.maximum = {512, 256, 128};
  bake::Config config;
  config.maxProbes = 512;
  auto grid = bake::place(scene, config);
  require(grid.points.size() <= 512 && grid.tasks.size() == grid.points.size(),
          "Probe placement respects the volume budget and includes unpartitioned space");
  require(grid.header.originSpacing[3] >= config.spacing / scene.sceneScale,
          "Grid spacing is expressed in source world units");
  require(grid.header.originSpacing[3] < config.spacing / scene.sceneScale * 2,
          "Budget fitting avoids the former full doubling of probe spacing");
  {
    const lightmap::Layout layout;
    const auto plain = bake::placeSparse(scene, config, layout);
    require(!bake::indexed(plain.header) && plain.lookup.empty() &&
                plain.header.originSpacing == grid.header.originSpacing &&
                plain.header.dimensions == grid.header.dimensions && plain.tasks == grid.tasks,
            "Hosts without a closed spatial partition retain complete dense probe coverage");
  }
  {
    scene::BakeScene cube;
    cube.sceneScale = 1;
    cube.hasBounds = true;
    cube.minimum = {0, 0, 0};
    cube.maximum = {1000, 1000, 1000};
    bake::Config budget;
    budget.spacing = 1;
    budget.maxProbes = 32768;
    const auto dense = bake::place(cube, budget);
    require(dense.header.dimensions == bake::UInt4{32, 32, 32, 32768},
            "A cubic volume uses the full 32-cubed allocation instead of a coarse power-of-two grid");
    require(dense.header.originSpacing[3] > 34 && dense.header.originSpacing[3] < 35,
            "The fitting density lies between the former 32/64 spacing steps");
    for (uint32_t c = 0; c < 3; ++c) {
      require(dense.points.front().position[c] < cube.minimum[c] &&
              dense.points.back().position[c] > cube.maximum[c],
              "Fitted grids still enclose the entire receiver volume with an interpolation border");
    }
    const auto repeated = bake::place(cube, budget);
    require(dense.header.originSpacing == repeated.header.originSpacing &&
            dense.header.dimensions == repeated.header.dimensions,
            "Repeated planning preserves probe locations and dimensions");
    budget.spacing = 100;
    const auto requested = bake::place(cube, budget);
    require(requested.header.originSpacing[3] == 100 && requested.points.size() == 2197,
            "An already fitting requested density is not made unnecessarily finer");
    cube.maximum = {1000, 17, 0};
    for (const uint32_t limit : {64u, 127u, 512u, 4096u}) {
      budget.maxProbes = limit;
      budget.spacing = 1;
      const auto thin = bake::place(cube, budget);
      require(thin.points.size() <= limit && thin.header.dimensions[2] == 3,
              "Flat and anisotropic volumes respect the exact allocation cap");
    }
  }
  for (auto &probe : grid.probes) {
    probe.statistics[3] = 1;
    probe.irradiance[0] = {2.f / .2820947918f, 1.f / .2820947918f, .5f / .2820947918f, 0};
    for (auto &moment : probe.moments)
      moment = {10000, 100000000, 0, 1};
  }
  for (const Vec3 n : {Vec3{0, 0, 1}, Vec3{0, 1, 0}, Vec3{1, 0, 0}}) {
    auto value = bake::sample(grid, {0, 0, 0}, n);
    require(value && std::abs((*value)[0] - 2) < 1e-5f && std::abs((*value)[2] - .5f) < 1e-5f,
            "Constant irradiance reconstructs independently of position and normal");
  }
  require(!bake::sampleSunVisibility(grid, {0, 0, 0}, {0, 0, 1}),
          "Indirect probes without direct visibility retain the shadow-map fallback");
  for (auto &probe : grid.probes) {
    probe.irradiance[7][3] = 1;
    probe.irradiance[8][3] = .25f;
  }
  const auto visibility = bake::sampleSunVisibility(grid, {0, 0, 0}, {0, 0, 1});
  require(visibility && std::abs(*visibility - .25f) < 1e-6f,
          "Static direct visibility interpolates separately from RGB irradiance");
  const auto unchanged = bake::sample(grid, {0, 0, 0}, {0, 0, 1});
  require(unchanged && std::abs((*unchanged)[0] - 2) < 1e-5f,
          "Probe visibility metadata cannot double-count direct diffuse energy");
  require(!bake::sampleSunVisibility(grid, {100000, 0, 0}, {0, 0, 1}),
          "Uncovered sunlight receivers retain complete shadow maps");
  require(!bake::sample(grid, {100000, 0, 0}, {0, 0, 1}), "Outside surfaces use the ambient fallback");
  for (auto &probe : grid.probes)
    probe.statistics[3] = 0;
  require(!bake::sample(grid, {0, 0, 0}, {0, 0, 1}), "Unusable probes cannot darken fallback lighting");
  for (auto &probe : grid.probes) {
    probe.statistics[3] = 1;
    for (auto &moment : probe.moments)
      moment = {0, 0, 1, 1};
  }
  require(!bake::sample(grid, {13, 17, 19}, {0, 0, 1}),
          "Depth moments reject distant samples hidden behind probe occluders");
  auto partition = std::make_shared<scene::VisibilityPartition>();
  partition->closed = true;
  partition->clusterCount = 1;
  partition->nodes.push_back({{1, 0, 0, 0}, {-1, -2}, {0, 0}});
  partition->leafClusters = {0, -1};
  scene.partition = partition;
  grid = bake::place(scene, config);
  require(grid.tasks.size() < grid.points.size(), "Solid BSP leaves consume no tracing work");
  for (const auto index : grid.tasks)
    require(grid.points[index].position[0] >= 0, "Every queued probe occupies a playable leaf");
  {
    const auto dense = grid;
    lightmap::Layout layout;
    auto compact = bake::placeSparse(scene, config, layout);
    require(bake::indexed(compact.header) && compact.points.size() <= config.maxProbes &&
                compact.tasks.size() == compact.points.size() && compact.lookup.size() > compact.points.size(),
            "Sparse BSP grids budget stored records instead of empty lattice slots");
    require(compact.header.originSpacing[3] < dense.header.originSpacing[3],
            "Recovered empty slots provide a finer physical lattice within the same record budget");
    compact.header.settings[2] = 3;
    compact.points.back().position[0] += compact.header.originSpacing[3] * .1f;
    auto expanded = compact;
    expanded.header.settings[2] -= 2;
    expanded.header.dimensions[3] = uint32_t(compact.lookup.size());
    expanded.probes.assign(compact.lookup.size(), {});
    expanded.lookup.clear();
    for (uint32_t i = 0; i < compact.probes.size(); ++i) {
      bake::Result value;
      value.statistics[3] = 1;
      value.irradiance[0] = {float(i % 7), float(i % 11), 1, 0};
      value.irradiance[7][3] = 1;
      value.irradiance[8][3] = float(i % 5) / 4;
      for (auto &moment : value.moments) moment = {100000, 1e10f, 0, 1};
      bake::storeProbe(compact, i, value);
      expanded.probes[compact.nodes[i]] = compact.probes[i];
      require(compact.lookup[compact.nodes[i]] == i + 1,
              "Compacted records preserve the inverse lattice mapping for relocated origins");
    }
    require(std::abs(compact.probes.back().irradiance[0][3] - compact.header.originSpacing[3] * .1f) < .001f,
            "Relocation offsets use virtual node coordinates instead of compact record indices");
    for (float x : {-100.f, 0.f, 50.f, 200.f})
      for (Vec3 normal : {Vec3{0, 0, 1}, Vec3{1, 0, 0}}) {
        const Vec3 position{x, 11, 7};
        const auto a = bake::sample(compact, position, normal), b = bake::sample(expanded, position, normal);
        require(bool(a) == bool(b), "Missing compact corners preserve dense-grid fallback coverage");
        if (a) for (uint32_t c = 0; c < 3; ++c)
          require(std::abs((*a)[c] - (*b)[c]) < 1e-6f, "Sparse addressing preserves irradiance weights");
        require(bake::sampleSunVisibility(compact, position, normal) ==
                    bake::sampleSunVisibility(expanded, position, normal),
                "Sparse addressing preserves independent sun visibility");
      }
    std::stop_source stop;
    stop.request_stop();
    bool refused = false;
    try { bake::placeSparse(scene, config, layout, stop.get_token()); }
    catch (const std::runtime_error &) { refused = true; }
    require(refused, "Superseded sparse fitting stops before allocating a lattice");
  }
  {
    // Two playable regions separated by a solid slab: a receiver can rescue
    // an otherwise untraced lattice node without moving the existing probes.
    bake::Grid moved;
    moved.header.originSpacing = {-1, -1, -1, 2};
    moved.header.dimensions = {2, 2, 2, 8};
    moved.header.settings = {0, 1, 0, 0};
    moved.points.resize(8);
    moved.probes.resize(8);
    for (uint32_t i = 0; i < 8; ++i)
      moved.points[i].position = {i & 1 ? 1.f : -1.f, i & 2 ? 1.f : -1.f, i & 4 ? 1.f : -1.f, 0};
    moved.tasks = {1, 3, 5, 7};
    scene::BakeScene region;
    region.sceneScale = 1;
    auto split = std::make_shared<scene::VisibilityPartition>();
    split->closed = true;
    split->clusterCount = 1;
    split->nodes.push_back({{1, 0, 0, -.5f}, {-1, -2}, {0, 0}});
    split->leafClusters = {0, -1};
    region.partition = split;
    lightmap::Layout receivers;
    receivers.points = {{{-.4f, -1, -1, 0}, {1, 0, 0, 0}},
                        {{-.4f, 1, -1, 0}, {1, 0, 0, 0}},
                        {{-.4f, -1, 1, 0}, {1, 0, 0, 0}},
                        {{-.4f, 1, 1, 0}, {1, 0, 0, 0}}};
    receivers.receiverSamples = {1, 1, 1, 0};
    const auto original = moved.points;
    require(bake::relocate(moved, region, receivers) == 3 && moved.tasks.size() == 7 && moved.points.size() == 8,
            "Relocation reuses only unused slots within the existing allocation");
    require(moved.points[1].position == original[1].position && moved.points[6].position == original[6].position,
            "Existing usable probes and donor-only receivers are not moved");
    for (auto index : moved.tasks) {
      bake::Result value;
      value.statistics[3] = 1;
      value.irradiance[0] = {2.f / .2820947918f, 0, 0, 0};
      for (auto &moment : value.moments) moment = {100, 10000, 0, 1};
      bake::storeProbe(moved, index, value);
    }
    require(moved.header.settings[2] == 1 && std::abs(moved.probes[0].irradiance[0][3] - .8f) < 1e-6f,
            "Actual trace displacement survives result publication in reserved grid alpha");
    const auto light = bake::sample(moved, {-.3f, -.5f, -.5f}, {1, 0, 0});
    require(light && std::abs((*light)[0] - 2) < 1e-5f,
            "Relocated interpolation preserves constant irradiance");
    require(bake::relocate(moved, region, receivers) == 0, "Repeated relocation does not move already queued probes");
  }
  config.maxProbes = 64;
  require(bake::place(scene, config).points.size() <= 64, "Coarsening reaches the smallest supported budget");
  config.spacing = std::numeric_limits<float>::quiet_NaN();
  bool rejected = false;
  try {
    bake::place(scene, config);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "Invalid spacing is rejected before allocation");
  std::puts("Irradiance placement, BSP filtering, reconstruction and occlusion passed");
}
