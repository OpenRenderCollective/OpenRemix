#include "forward/bake/probe_receiver_geometry.h"
#include "scene/bake_scene.h"
#include <algorithm>
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
}
