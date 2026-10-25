#include "forward/bake/adaptive_grid.h"
#include "forward/bake/spherical_harmonics.h"
#include "forward/bake/refinement_builder.h"
#include "forward/bake/receiver_clearance.h"
#include "forward/lightmap/layout.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace remix::forward::bake {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Probe refinement superseded");
    }
    std::array<uint32_t, 3> coordinate(uint32_t node, const UInt4 &dimensions) {
      std::array<uint32_t, 3> result;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        result[axis] = node % dimensions[axis];
        node /= dimensions[axis];
      }
      return result;
    }
    double cellVariation(const Grid &grid, uint32_t cell, double reference) {
      const auto &h = grid.header;
      const auto base = coordinate(cell, h.dimensions);
      std::array<double, 27> sum{}, squared{};
      std::array<float, 6> low, high;
      low.fill(1); high.fill(0);
      uint32_t count = 0;
      for (uint32_t corner = 0; corner < 8; ++corner) {
        const auto x = base[0] + (corner & 1), y = base[1] + ((corner >> 1) & 1),
                   z = base[2] + ((corner >> 2) & 1);
        const uint32_t record = grid.lookup[(z * h.dimensions[1] + y) * h.dimensions[0] + x];
        if (!record) continue;
        const auto &probe = grid.probes[record - 1];
        if (probe.statistics[3] < .5f) continue;
        ++count;
        for (uint32_t j = 0; j < 9; ++j) for (uint32_t c = 0; c < 3; ++c) {
          const double value = probe.irradiance[j][c];
          sum[j * 3 + c] += value;
          squared[j * 3 + c] += value * value;
        }
        for (uint32_t face = 0; face < 6; ++face) {
          low[face] = std::min(low[face], probe.moments[face][2]);
          high[face] = std::max(high[face], probe.moments[face][2]);
        }
      }
      if (!count) return 2; // Geometry can reveal a room missed by every coarse corner.
      double variance = 0, skyRange = 0;
      for (uint32_t j = 0; j < 27; ++j)
        variance += std::max(0., squared[j] / count - sum[j] * sum[j] / (double(count) * count));
      for (uint32_t face = 0; face < 6; ++face) skyRange = std::max(skyRange, double(high[face] - low[face]));
      return std::sqrt(variance) / reference + skyRange;
    }
    double receiverError(const Grid &grid, const lightmap::Layout &layout,
                         std::span<const Result> reference, std::span<const uint32_t> receivers) {
      // A fixed number of deterministic samples per cell bounds CPU work and
      // gives small rooms the same opportunity as large outdoor surfaces.
      const size_t count = std::min<size_t>(16, receivers.size());
      double sum = 0;
      for (size_t i = 0; i < count; ++i) {
        const uint32_t receiver = receivers[(i * 2 + 1) * receivers.size() / (count * 2)];
        const auto &point = layout.points[receiver];
        const Vec3 normal = normalized({point.normal[0], point.normal[1], point.normal[2]});
        const auto estimate = sample(grid, {point.position[0], point.position[1], point.position[2]}, normal);
        if (!estimate) { sum += 1; continue; }
        const auto basis = irradianceBasis(normal);
        Vec3 expected{};
        for (uint32_t j = 0; j < 9; ++j)
          for (uint32_t c = 0; c < 3; ++c) expected[c] += reference[receiver].irradiance[j][c] * basis[j];
        double difference = 0, energy = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          const double a = std::max(0.f, expected[c]), b = (*estimate)[c];
          difference += (a - b) * (a - b);
          energy += a * a + b * b;
        }
        sum += energy > 1e-12 ? difference / energy : 0;
      }
      return sum / count;
    }
  }
  Grid placeRefinementBase(const scene::BakeScene &scene, const Config &config,
                            const lightmap::Layout &layout, Refinement &refinement,
                            std::stop_token cancelled) {
    refinement = {};
    if (scene.partition && scene.partition->closed && !layout.points.empty() && config.maxProbes >= 128) {
      auto coarse = config;
      coarse.maxProbes /= 2;
      // The global fallback need not spend the near-surface density budget on
      // distant air. An eightfold patch can still reach the requested spacing.
      coarse.spacing *= 8;
      try {
        auto grid = placeSparse(scene, coarse, layout, cancelled);
        uint32_t factor = 1;
        while (factor < 8) {
          const uint32_t next = factor * 2, side = next + 1;
          if (grid.header.originSpacing[3] * scene.sceneScale / next < config.spacing ||
              side * side * side > config.maxProbes - grid.probes.size()) break;
          factor = next;
        }
        if (factor > 1) {
          refinement = {factor, config.maxProbes, config.relocateProbes};
          return grid;
        }
      } catch (const EmptyProbePlacement &) {
        // Coarse alignment can miss an entire narrow interior. Retry ordinary
        // placement at the requested density and full budget, not an empty bake.
      }
    }
    return placeSparse(scene, config, layout, cancelled);
  }
  uint32_t refineGrid(Grid &grid, const scene::BakeScene &scene, const lightmap::Layout &layout,
                       const Refinement &refinement, std::span<const Result> surfaceReference,
                       std::stop_token cancelled) {
    if (!refinement.factor) return 0;
    if (!indexed(grid.header) || adaptive(grid.header) || !scene.partition || !scene.partition->closed ||
        !std::isfinite(scene.sceneScale) || scene.sceneScale <= 0 ||
        grid.points.size() != grid.probes.size() || grid.origins.size() != grid.points.size() ||
        grid.probes.size() > refinement.recordBudget ||
        (refinement.factor != 2 && refinement.factor != 4 && refinement.factor != 8) ||
        (!layout.receiverSamples.empty() && layout.receiverSamples.size() != layout.points.size()) ||
        (!surfaceReference.empty() && surfaceReference.size() != layout.points.size()) ||
        layout.points.size() > UINT32_MAX)
      throw std::invalid_argument("Invalid adaptive irradiance inputs");
    check(cancelled);
    const bool timing = std::getenv("REMIX_DEBUG_BAKE_TIMING") != nullptr;
    const auto began = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto &h = grid.header;
    const uint32_t nodes = gridNodes(h);
    const float spacing = h.originSpacing[3] / refinement.factor;
    const float clearance = std::min(.25f / scene.sceneScale, spacing * .1f);
    std::vector<std::vector<uint32_t>> receivers(nodes);
    for (uint32_t i = 0; i < layout.points.size(); ++i) {
      if ((i & 1023u) == 0) check(cancelled);
      if (!layout.receiverSamples.empty() && !layout.receiverSamples[i]) continue;
      const auto &point = layout.points[i];
      const Vec3 normal = normalized({point.normal[0], point.normal[1], point.normal[2]});
      const auto contained = partitionContainedBias(scene,
          {point.position[0], point.position[1], point.position[2]}, normal, clearance);
      if (!contained) continue;
      std::array<uint32_t, 3> base;
      bool inside = true;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const float q = (point.position[axis] + normal[axis] * *contained - h.originSpacing[axis]) /
                        h.originSpacing[3];
        if (!std::isfinite(q) || q < 0 || q >= h.dimensions[axis] - 1) { inside = false; break; }
        base[axis] = uint32_t(q);
      }
      if (inside) receivers[(base[2] * h.dimensions[1] + base[1]) * h.dimensions[0] + base[0]].push_back(i);
    }
    const auto binned = timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    double energy = 0;
    uint32_t valid = 0;
    for (const auto &probe : grid.probes) {
      if (probe.statistics[3] < .5f) continue;
      ++valid;
      for (const auto &coefficient : probe.irradiance)
        for (uint32_t c = 0; c < 3; ++c) energy += double(coefficient[c]) * coefficient[c];
    }
    const double reference = std::max(1e-4, std::sqrt(energy / std::max(valid, 1u)));
    struct Candidate { uint32_t cell; double score; };
    std::vector<Candidate> candidates;
    for (uint32_t cell = 0; cell < nodes; ++cell)
      if (!receivers[cell].empty()) {
        if ((cell & 63u) == 0) check(cancelled);
        const double score = surfaceReference.empty()
            ? cellVariation(grid, cell, reference)
            : receiverError(grid, layout, surfaceReference, receivers[cell]);
        if (!std::isfinite(score)) throw std::invalid_argument("Non-finite coarse probe variation");
        candidates.push_back({cell, score});
      }
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
      return a.score != b.score ? a.score > b.score : a.cell < b.cell;
    });
    std::vector<uint32_t> priority;
    priority.reserve(candidates.size());
    for (const auto &candidate : candidates) priority.push_back(candidate.cell);
    if (timing)
      std::fprintf(stderr, "Forward refinement priority: %zu receivers, %zu candidate cells, "
                           "bin_ms=%.3f score_ms=%.3f\n", layout.points.size(), priority.size(),
                   std::chrono::duration<double, std::milli>(binned - began).count(),
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - binned).count());
    return buildRefinement(grid, scene, layout, refinement, receivers, priority, cancelled);
  }
} // namespace remix::forward::bake
