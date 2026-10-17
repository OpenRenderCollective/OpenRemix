#include "forward/bake/grid.h"
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
