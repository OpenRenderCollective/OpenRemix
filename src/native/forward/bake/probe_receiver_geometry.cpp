#include "forward/bake/probe_receiver_geometry.h"
#include "scene/bake_scene.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace remix::forward::bake {
  namespace {
    struct ChartRange { uint32_t draw; size_t first, end; };
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver geometry scan superseded");
    }
    size_t endOf(const scene::Draw &draw) {
      if (!draw.surface) throw std::invalid_argument("Receiver draw has no surface");
      const size_t count = draw.indexCount ? draw.indexCount : draw.surface->indices.size();
      if (draw.firstIndex > draw.surface->indices.size() || count > draw.surface->indices.size() - draw.firstIndex ||
          count % 3 || count > UINT32_MAX || uint64_t(draw.firstIndex) + count > UINT32_MAX)
        throw std::invalid_argument("Invalid receiver draw range");
      return draw.firstIndex + count;
    }
  }
  std::vector<ReceiverRange> unchartedReceiverRanges(const scene::BakeScene &source, std::stop_token cancelled) {
    check(cancelled);
    if (source.draws.size() > UINT32_MAX) throw std::invalid_argument("Too many receiver draws");
    std::vector<ChartRange> charts;
    charts.reserve(source.charts.size());
    for (const auto &chart : source.charts) {
      check(cancelled);
      if (chart.draw >= source.draws.size() || chart.texcoords.empty() || chart.texcoords.size() % 3)
        throw std::invalid_argument("Invalid receiver chart range");
      const auto &draw = source.draws[chart.draw];
      const size_t end = endOf(draw);
      if (chart.firstIndex < draw.firstIndex || chart.firstIndex > end ||
          (chart.firstIndex - draw.firstIndex) % 3 || chart.texcoords.size() > end - chart.firstIndex)
        throw std::invalid_argument("Receiver chart lies outside its draw triangles");
      charts.push_back({chart.draw, chart.firstIndex, chart.firstIndex + chart.texcoords.size()});
    }
    std::sort(charts.begin(), charts.end(), [](const auto &a, const auto &b) {
      return std::tie(a.draw, a.first) < std::tie(b.draw, b.first);
    });
    for (size_t i = 1; i < charts.size(); ++i)
      if (charts[i].draw == charts[i - 1].draw && charts[i].first < charts[i - 1].end)
        throw std::invalid_argument("Overlapping receiver chart ranges");
    std::vector<ReceiverRange> result;
    auto chart = charts.begin();
    for (uint32_t drawIndex = 0; drawIndex < source.draws.size(); ++drawIndex) {
      check(cancelled);
      while (chart != charts.end() && chart->draw < drawIndex) ++chart;
      const auto &draw = source.draws[drawIndex];
      if (!opaqueProbeReceiver(draw)) continue;
      size_t first = draw.firstIndex;
      const size_t end = endOf(draw);
      auto append = [&](size_t last) {
        if (last > first) result.push_back({drawIndex, uint32_t(first), uint32_t(last - first)});
      };
      for (; chart != charts.end() && chart->draw == drawIndex; ++chart) {
        append(chart->first);
        first = chart->end;
      }
      append(end);
    }
    return result;
  }
  bool playableReceiverTriangle(const scene::BakeScene &source,
      const std::array<std::array<float, 3>, 3> &positions, std::stop_token cancelled) {
    check(cancelled);
    for (const auto &p : positions)
      for (float value : p)
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite receiver triangle");
    if (!source.partition || !source.partition->closed) return true;
    const auto &partition = *source.partition;
    if (partition.nodes.empty()) return true;
    std::array<float, 3> centre;
    for (uint32_t axis = 0; axis < 3; ++axis)
      centre[axis] = float((double(positions[0][axis]) + positions[1][axis] + positions[2][axis]) / 3);
    if (partition.cluster(centre.data()) >= 0) return true;
    for (const auto &p : positions) if (partition.cluster(p.data()) >= 0) return true;
    using Point = std::array<double, 3>;
    struct Fragment { int32_t node; std::vector<Point> polygon; };
    Fragment root{0, {}};
    for (const auto &p : positions) root.polygon.push_back({p[0], p[1], p[2]});
    std::vector<Fragment> pending;
    pending.push_back(std::move(root));
    uint32_t visited = 0;
    while (!pending.empty()) {
      check(cancelled);
      if (++visited > 4096) return true;
      auto fragment = std::move(pending.back()); pending.pop_back();
      if (fragment.node < 0) {
        const auto leaf = uint64_t(-1 - int64_t(fragment.node));
        if (leaf >= partition.leafClusters.size()) return true;
        const auto cluster = partition.leafClusters[size_t(leaf)];
        if (cluster >= 0) return true;
        continue;
      }
      if (size_t(fragment.node) >= partition.nodes.size()) return true;
      const auto &node = partition.nodes[size_t(fragment.node)];
      std::vector<double> distances;
      bool front = false, back = false;
      for (const auto &p : fragment.polygon) {
        const double distance = p[0] * node.plane[0] + p[1] * node.plane[1] + p[2] * node.plane[2] - node.plane[3];
        if (!std::isfinite(distance)) return true;
        distances.push_back(distance);
        front |= distance >= 0; back |= distance < 0;
      }
      if (!front || !back) {
        fragment.node = node.children[front ? 0 : 1];
        pending.push_back(std::move(fragment));
        continue;
      }
      for (uint32_t side = 0; side < 2; ++side) {
        Fragment clipped{node.children[side], {}};
        for (size_t i = 0; i < fragment.polygon.size(); ++i) {
          const size_t j = (i + 1) % fragment.polygon.size();
          const bool inside = side ? distances[i] <= 0 : distances[i] >= 0;
          const bool nextInside = side ? distances[j] <= 0 : distances[j] >= 0;
          if (inside) clipped.polygon.push_back(fragment.polygon[i]);
          if (inside == nextInside) continue;
          const double t = distances[i] / (distances[i] - distances[j]);
          Point contact;
          for (uint32_t axis = 0; axis < 3; ++axis)
            contact[axis] = fragment.polygon[i][axis] + t * (fragment.polygon[j][axis] - fragment.polygon[i][axis]);
          clipped.polygon.push_back(contact);
        }
        if (clipped.polygon.size() >= 3) pending.push_back(std::move(clipped));
      }
    }
    return false;
  }

}
