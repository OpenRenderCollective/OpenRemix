#include "forward/bake/model_receivers.h"
#include "forward/bake/probe_receiver_geometry.h"
#include "forward/material.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace remix::forward::bake {
  namespace {
    struct Range { uint32_t draw; size_t first, end; };
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Model receiver sampling superseded");
    }
    size_t indexCount(const scene::Draw &draw) {
      const auto &surface = *draw.surface;
      const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
      if (draw.firstIndex > surface.indices.size() || count > surface.indices.size() - draw.firstIndex || count % 3)
        throw std::invalid_argument("Invalid model receiver draw range");
      return count;
    }
    struct Candidate { ModelReceiver receiver; double area; };
  }
  std::vector<ModelReceiver> modelReceivers(const scene::BakeScene &source,
      uint32_t maximumSamples, std::stop_token cancelled) {
    check(cancelled);
    if (!maximumSamples || maximumSamples > 4096 || source.draws.size() > UINT32_MAX)
      throw std::invalid_argument("Model receiver budget is 1..4096");
    std::vector<Range> ranges;
    // Coverage belongs to indexed draw ranges, not whole shared meshes.
    ranges.reserve(source.charts.size());
    for (const auto &chart : source.charts) {
      check(cancelled);
      if (chart.draw >= source.draws.size() || !source.draws[chart.draw].surface ||
          chart.texcoords.empty() || chart.firstIndex % 3 || chart.texcoords.size() % 3)
        throw std::invalid_argument("Invalid model receiver chart range");
      const auto &draw = source.draws[chart.draw];
      const size_t end = draw.firstIndex + indexCount(draw);
      if (chart.firstIndex < draw.firstIndex || chart.firstIndex > end || chart.texcoords.size() > end - chart.firstIndex)
        throw std::invalid_argument("Model receiver chart lies outside its draw");
      ranges.push_back({chart.draw, chart.firstIndex, chart.firstIndex + chart.texcoords.size()});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) {
      return std::tie(a.draw, a.first) < std::tie(b.draw, b.first);
    });
    for (size_t i = 1; i < ranges.size(); ++i)
      if (ranges[i].draw == ranges[i - 1].draw && ranges[i].first < ranges[i - 1].end)
        throw std::invalid_argument("Overlapping model receiver chart ranges");
    auto chartRange = [&](uint32_t draw) {
      const auto first = std::lower_bound(ranges.begin(), ranges.end(), draw,
          [](const auto &range, uint32_t value) { return range.draw < value; });
      const auto end = std::upper_bound(first, ranges.end(), draw,
          [](uint32_t value, const auto &range) { return value < range.draw; });
      return std::pair{first, end};
    };
    std::vector<uint32_t> eligible;
    for (uint32_t i = 0; i < source.draws.size(); ++i) {
      check(cancelled);
      const auto &draw = source.draws[i];
      if (!opaqueProbeReceiver(draw)) continue;
      const size_t count = indexCount(draw);
      const auto [first, end] = chartRange(i);
      size_t covered = 0;
      for (auto r = first; r != end; ++r) covered += r->end - r->first;
      if (count > covered) eligible.push_back(i);
    }
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
      const auto [firstChart, lastChart] = chartRange(drawIndex);
      auto chart = firstChart;
      std::array<std::optional<Candidate>, 6> directions;
      const size_t end = draw.firstIndex + indexCount(draw);
      for (size_t first = draw.firstIndex; first < end; first += 3) {
        if (first % 1024 < 3) check(cancelled);
        while (chart != lastChart && chart->end <= first) ++chart;
        if (chart != lastChart && chart->first <= first && first + 3 <= chart->end) continue;
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
