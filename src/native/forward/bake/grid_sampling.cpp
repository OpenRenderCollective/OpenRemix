#include "forward/bake/grid.h"
#include <algorithm>
#include <cmath>

namespace remix::forward::bake {
  ProbeStencil probeStencil(const Grid &grid, Vec3 position, Vec3 normal) {
    ProbeStencil result;
    const auto &h = grid.header;
    if (!h.dimensions[3]) return result;
    std::array<float, 2> confidence{};
    auto cell = [&](uint32_t first, UInt4 dimensions, std::array<uint32_t, 3> start,
                    uint32_t divisor, uint32_t directory) {
      const float spacing = h.originSpacing[3] / divisor, ratio = 1.f / divisor;
      const float bias = h.settings[0] * ratio, minimumVariance = h.settings[1] * ratio * ratio;
      std::array<uint32_t, 3> base;
      Vec3 fraction, biased = position;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        biased[axis] += normal[axis] * bias;
        const float origin = h.originSpacing[axis] + start[axis] * spacing;
        const float q = (biased[axis] - origin) / spacing;
        if (!std::isfinite(q) || q < 0 || q > dimensions[axis] - 1) return 0.f;
        base[axis] = std::min(uint32_t(q), dimensions[axis] - 2);
        fraction[axis] = q - base[axis];
      }
      float total = 0, support = 0;
      for (uint32_t corner = 0; corner < 8; ++corner) {
        std::array<uint32_t, 3> coordinate;
        Vec3 delta;
        float weight = 1;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const bool upper = (corner >> axis) & 1;
          coordinate[axis] = base[axis] + upper;
          weight *= upper ? fraction[axis] : 1 - fraction[axis];
          delta[axis] = biased[axis] - (h.originSpacing[axis] + (start[axis] + coordinate[axis]) * spacing);
        }
        uint32_t index = (coordinate[2] * dimensions[1] + coordinate[1]) * dimensions[0] + coordinate[0];
        if (indexed(h)) {
          const auto record = grid.lookup.at(directory + index);
          if (!record) continue;
          index = record - 1;
        }
        result.indices[first + corner] = index;
        const auto &probe = grid.probes[index];
        if (probe.statistics[3] < .5f || weight <= 0) continue;
        if (h.settings[2] > .5f)
          for (uint32_t axis = 0; axis < 3; ++axis) delta[axis] -= probe.irradiance[axis][3];
        const float distance = std::sqrt(dot(delta, delta));
        float visibility = 1;
        if (const auto directional = grid.depth.visibility(index, delta, minimumVariance)) visibility = *directional;
        else {
          uint32_t axis = 0;
          for (uint32_t c = 1; c < 3; ++c) if (std::abs(delta[c]) > std::abs(delta[axis])) axis = c;
          const auto &moment = probe.moments[axis * 2 + uint32_t(delta[axis] < 0)];
          if (moment[3] <= 0) continue;
          if (distance > moment[0]) {
            const float variance = std::max(minimumVariance, moment[1] - moment[0] * moment[0]);
            const float difference = distance - moment[0];
            visibility = std::pow(variance / (variance + difference * difference), 3.f);
          }
        }
        const float facing = distance > 1e-6f ? dot(normal, delta) / distance : -1;
        const float facingWeight = std::pow(std::max(.05f, .5f - .5f * facing), 2.f);
        support += weight * facingWeight;
        weight *= visibility;
        weight *= facingWeight;
        result.weights[first + corner] = weight;
        total += weight;
      }
      confidence[first / 8] = support > 1e-6f ? total / support : 0;
      return total;
    };
    result.count = 8;
    const float coarseTotal = cell(0, h.dimensions, {}, 1, 0);
    if (!adaptive(h)) return result;
    const uint32_t nodes = gridNodes(h);
    std::array<uint32_t, 3> parent, start;
    Vec3 rootPosition;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const float q = (position[axis] - h.originSpacing[axis]) / h.originSpacing[3];
      if (!std::isfinite(q) || q < 0 || q >= h.dimensions[axis] - 1) return result;
      parent[axis] = uint32_t(q);
      rootPosition[axis] = q;
    }
    uint32_t index = (parent[2] * h.dimensions[1] + parent[1]) * h.dimensions[0] + parent[0];
    auto reference = grid.lookup.at(nodes + 4 + index);
    const uint32_t factor = patchFactor(reference, grid.lookup.at(nodes + 1));
    const std::array<uint32_t, 3> strides{1, h.dimensions[0], h.dimensions[0] * h.dimensions[1]};
    // Bias can cross a stitched face. Sample the adjacent compatible patch
    // rather than dropping its fine contribution in a thin strip at the edge.
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const float biased = (position[axis] + normal[axis] * h.settings[0] / factor - h.originSpacing[axis]) /
                            h.originSpacing[3];
      const bool low = biased < parent[axis], high = biased >= parent[axis] + 1;
      if (low && (patchFaces(reference) & (1u << (axis * 2)))) { --parent[axis]; index -= strides[axis]; }
      else if (high && (patchFaces(reference) & (1u << (axis * 2 + 1)))) { ++parent[axis]; index += strides[axis]; }
      else continue;
      reference = grid.lookup.at(nodes + 4 + index);
    }
    const uint32_t block = patchOffset(reference), faces = patchFaces(reference);
    float edge = 1;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      start[axis] = parent[axis] * factor;
      const float fraction = rootPosition[axis] - parent[axis];
      if (!(faces & (1u << (axis * 2)))) edge = std::min(edge, fraction);
      if (!(faces & (1u << (axis * 2 + 1)))) edge = std::min(edge, 1 - fraction);
    }
    float blend = std::clamp(edge * factor, 0.f, 1.f);
    blend = blend * blend * (3 - 2 * blend);
    if (!block || blend <= 0) return result;
    const uint32_t side = factor + 1;
    const float fineTotal = cell(8, {side, side, side, 0}, start, factor, block);
    if (fineTotal < 1e-4f) return result;
    if (coarseTotal < 1e-4f) blend = 1;
    else if (!grid.depth.empty()) {
      // Do not normalize a tiny surviving blocked coarse stencil back to its
      // full blend share. Confidence changes the mixture, never constant energy.
      const float fine = blend * confidence[1], coarse = (1 - blend) * confidence[0];
      if (fine + coarse > 1e-6f) blend = fine / (fine + coarse);
    }
    for (uint32_t corner = 0; corner < 8; ++corner) {
      result.weights[corner] *= coarseTotal >= 1e-4f ? (1 - blend) / coarseTotal : 0;
      result.weights[8 + corner] *= blend / fineTotal;
    }
    result.count = 16;
    return result;
  }
} // namespace remix::forward::bake
