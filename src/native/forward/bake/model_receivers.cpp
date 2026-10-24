#include "forward/bake/model_receivers.h"
#include "forward/bake/probe_receiver_geometry.h"
#include "forward/material.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Model receiver sampling superseded");
    }
    struct Candidate { ModelReceiver receiver; double area; };
  }
  std::vector<ModelReceiver> modelReceivers(const scene::BakeScene &source,
      uint32_t maximumSamples, std::stop_token cancelled) {
    check(cancelled);
    if (!maximumSamples || maximumSamples > 4096 || source.draws.size() > UINT32_MAX)
      throw std::invalid_argument("Model receiver budget is 1..4096");
    const auto ranges = unchartedReceiverRanges(source, cancelled);
    std::vector<uint32_t> eligible;
    for (const auto &range : ranges)
      if (eligible.empty() || eligible.back() != range.draw) eligible.push_back(range.draw);
    const size_t selected = std::min<size_t>(maximumSamples, eligible.size());
    // Spread a limited budget across draws before considering extra directions.
    std::vector<std::vector<Candidate>> draws;
    draws.reserve(selected);
    for (size_t i = 0; i < selected; ++i) {
      check(cancelled);
      const uint32_t drawIndex = eligible[(i * 2 + 1) * eligible.size() / (selected * 2)];
      const auto &draw = source.draws[drawIndex];
      const auto &surface = *draw.surface;
      const auto transform = normalMatrix(draw.model);
      const auto firstRange = std::lower_bound(ranges.begin(), ranges.end(), drawIndex,
          [](const auto &range, uint32_t value) { return range.draw < value; });
      std::array<std::optional<Candidate>, 6> directions;
      for (auto range = firstRange; range != ranges.end() && range->draw == drawIndex; ++range)
      for (size_t first = range->firstIndex; first < size_t(range->firstIndex) + range->indexCount; first += 3) {
        if (first % 1024 < 3) check(cancelled);
        std::array<Vec3, 3> positions;
        Vec3 normal{};
        for (uint32_t c = 0; c < 3; ++c) {
          const int64_t index = int64_t(surface.indices[first + c]) + draw.vertexOffset;
          if (index < 0 || uint64_t(index) >= surface.vertices.size())
            throw std::invalid_argument("Invalid model receiver vertex index");
          const auto &vertex = surface.vertices[size_t(index)];
          positions[c] = transformPoint(draw.model, {vertex.position[0], vertex.position[1], vertex.position[2]});
          for (uint32_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(positions[c][axis])) throw std::invalid_argument("Nonfinite model receiver position");
            normal[axis] += vertex.normal[axis] / 3;
          }
        }
        std::array<double, 3> a, b, geometric;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          a[axis] = double(positions[1][axis]) - positions[0][axis];
          b[axis] = double(positions[2][axis]) - positions[0][axis];
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
          geometric[axis] = a[(axis + 1) % 3] * b[(axis + 2) % 3] - a[(axis + 2) % 3] * b[(axis + 1) % 3];
        const double area = std::hypot(geometric[0], geometric[1], geometric[2]);
        if (!(area > 0)) continue;
        normal = transformVector(transform, normal);
        const double length = std::hypot(double(normal[0]), double(normal[1]), double(normal[2]));
        if (!std::isfinite(length)) throw std::invalid_argument("Nonfinite model receiver normal");
        if (length > 1e-10) for (auto &value : normal) value = float(value / length);
        else for (uint32_t axis = 0; axis < 3; ++axis) normal[axis] = float(geometric[axis] / area);
        if (!std::all_of(normal.begin(), normal.end(), [](float value) { return std::isfinite(value); }))
          throw std::invalid_argument("Nonfinite model receiver normal");
        uint32_t axis = 0;
        for (uint32_t c = 1; c < 3; ++c) if (std::abs(normal[c]) > std::abs(normal[axis])) axis = c;
        const uint32_t direction = axis * 2 + uint32_t(normal[axis] < 0);
        if (directions[direction] && directions[direction]->area >= area) continue;
        Candidate candidate{{{}, drawIndex, uint32_t(first)}, area};
        for (uint32_t c = 0; c < 3; ++c) {
          candidate.receiver.point.position[c] = float((double(positions[0][c]) + positions[1][c] + positions[2][c]) / 3);
          candidate.receiver.point.normal[c] = normal[c];
        }
        if (source.partition && source.partition->closed &&
            source.partition->cluster(candidate.receiver.point.position.data()) < 0) continue;
        directions[direction] = candidate;
      }
      auto &chosen = draws.emplace_back();
      for (const auto &candidate : directions) if (candidate) chosen.push_back(*candidate);
      std::sort(chosen.begin(), chosen.end(), [](const auto &a, const auto &b) {
        return a.area != b.area ? a.area > b.area : a.receiver.firstIndex < b.receiver.firstIndex;
      });
    }
    std::vector<ModelReceiver> result;
    result.reserve(std::min<size_t>(maximumSamples, selected * 6));
    for (uint32_t round = 0; round < 6; ++round)
      for (const auto &draw : draws) {
        if (result.size() == maximumSamples) return result;
        if (round < draw.size()) result.push_back(draw[round].receiver);
      }
    return result;
  }
}
