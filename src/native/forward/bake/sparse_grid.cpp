#include "forward/bake/grid.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace remix::forward::bake {
  Grid placeSparse(const scene::BakeScene &scene, const Config &config, const lightmap::Layout &layout,
                   std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Sparse probe placement superseded");
    };
    auto prepare = [&](const Config &placement) {
      check();
      auto grid = place(scene, placement);
      if (config.relocateProbes) relocate(grid, scene, layout, cancelled);
      check();
      return grid;
    };
    // Unpartitioned hosts retain dense addressing; do not infer empty space
    // from missing visibility data or discard potential receivers.
    if (!scene.partition || !scene.partition->closed) return prepare(config);
    auto best = prepare(config);
    const uint32_t virtualLimit = uint32_t(std::min(uint64_t(maximumGridNodes), uint64_t(config.maxProbes) * 8));
    float upper = best.header.originSpacing[3] * scene.sceneScale;
    float lower = config.spacing;
    // Occupancy is not monotonic as lattice phase changes. Keep the best
    // verified fitting candidate, not just the last binary-search estimate.
    for (uint32_t trial = 0; trial < 8 && upper > lower * 1.001f; ++trial) {
      const float occupancy = float(best.tasks.size()) / config.maxProbes;
      const float estimate = upper * std::cbrt(std::clamp(occupancy, .01f, 1.f));
      Config candidate = config;
      candidate.maxProbes = virtualLimit;
      candidate.spacing = trial == 0 ? std::max(lower, estimate) : .5f * (lower + upper);
      auto grid = prepare(candidate);
      const float spacing = grid.header.originSpacing[3] * scene.sceneScale;
      if (spacing >= upper * .9999f) break;
      if (grid.tasks.size() <= config.maxProbes) {
        upper = spacing;
        best = std::move(grid);
        if (best.tasks.size() >= config.maxProbes * .995f) break;
      } else {
        lower = spacing;
      }
    }
    check();
    Grid result;
    result.header = best.header;
    result.header.settings[2] += 2; // bit 1: indexed, bit 0: relocated origins
    result.lookup.resize(best.points.size());
    result.nodes = std::move(best.tasks);
    result.points.reserve(result.nodes.size());
    for (uint32_t node : result.nodes) {
      result.lookup[node] = uint32_t(result.points.size()) + 1;
      result.points.push_back(best.points[node]);
    }
    result.probes.resize(result.points.size());
    result.tasks.resize(result.points.size());
    std::iota(result.tasks.begin(), result.tasks.end(), 0u);
    result.header.dimensions[3] = uint32_t(result.points.size());
    return result;
  }
} // namespace remix::forward::bake
