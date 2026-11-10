#include "scene/bake_scene.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace remix::scene {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface chart validation superseded");
    }
    using Uv = std::array<float, 2>;
    struct Triangle {
      std::array<Uv, 3> uv;
      Uv minimum, maximum;
    };
    // Separating-axis test for positive-area overlap. Shared vertices and
    // edges are valid, and subtexel folds still count as overlap.
    bool overlaps(const Triangle &a, const Triangle &b) {
      constexpr double epsilon = 1e-6; // luxels, independent of atlas resolution
      for (const auto *triangle : {&a, &b})
        for (uint32_t edge = 0; edge < 3; ++edge) {
          const auto &start = triangle->uv[edge], &end = triangle->uv[(edge + 1) % 3];
          const double x = double(end[1]) - start[1], y = double(start[0]) - end[0];
          double amin = INFINITY, amax = -INFINITY, bmin = INFINITY, bmax = -INFINITY;
          for (uint32_t c = 0; c < 3; ++c) {
            const double pa = x * a.uv[c][0] + y * a.uv[c][1];
            const double pb = x * b.uv[c][0] + y * b.uv[c][1];
            amin = std::min(amin, pa);
            amax = std::max(amax, pa);
            bmin = std::min(bmin, pb);
            bmax = std::max(bmax, pb);
          }
          if (std::min(amax, bmax) - std::max(amin, bmin) <= epsilon * std::hypot(x, y))
            return false;
        }
      return true;
    }
    void validateMapping(const BakeSurfaceChart &chart, std::stop_token cancelled) {
      std::vector<Triangle> triangles;
      for (size_t corner = 0; corner < chart.texcoords.size(); corner += 3) {
        if ((corner & 255u) == 0)
          check(cancelled);
        Triangle triangle;
        std::copy_n(chart.texcoords.begin() + corner, 3, triangle.uv.begin());
        const auto &a = triangle.uv[0], &b = triangle.uv[1], &c = triangle.uv[2];
        const double area =
            (double(b[0]) - a[0]) * (double(c[1]) - a[1]) - (double(b[1]) - a[1]) * (double(c[0]) - a[0]);
        if (std::abs(area) < 1e-12)
          continue; // collinear source fan triangles own no lightmap area
        for (uint32_t axis = 0; axis < 2; ++axis) {
          triangle.minimum[axis] = std::min({a[axis], b[axis], c[axis]});
          triangle.maximum[axis] = std::max({a[axis], b[axis], c[axis]});
        }
        triangles.push_back(triangle);
      }
      std::sort(triangles.begin(), triangles.end(),
                [](const auto &a, const auto &b) { return a.minimum[0] < b.minimum[0]; });
      std::vector<size_t> active;
      for (size_t i = 0; i < triangles.size(); ++i) {
        check(cancelled);
        const auto &candidate = triangles[i];
        active.erase(
            std::remove_if(active.begin(), active.end(),
                           [&](size_t j) { return triangles[j].maximum[0] <= candidate.minimum[0]; }),
            active.end());
        for (auto j : active) {
          const auto &other = triangles[j];
          if (other.maximum[1] <= candidate.minimum[1] || candidate.maximum[1] <= other.minimum[1])
            continue;
          if (overlaps(candidate, other))
            throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        }
        active.push_back(i);
      }
    }
  } // namespace
  void validateBakeCharts(const std::vector<Draw> &draws, const std::vector<BakeSurfaceChart> &charts,
                          std::stop_token cancelled) {
    check(cancelled);
    constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    struct Range {
      uint32_t draw, first;
      size_t end;
    };
    std::vector<Range> ranges;
    for (const auto &chart : charts) {
      if (chart.draw >= draws.size() || chart.width < 2 || chart.width > 4096 || chart.height < 2 ||
          chart.height > 4096 || chart.texcoords.empty() || chart.firstIndex % 3 ||
          chart.texcoords.size() % 3)
        throw invalid;
      const auto &draw = draws[chart.draw];
      if (!draw.surface)
        throw invalid;
      const auto &surface = *draw.surface;
      const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
      if (draw.firstIndex > surface.indices.size() || count > surface.indices.size() - draw.firstIndex ||
          chart.firstIndex < draw.firstIndex || chart.firstIndex > draw.firstIndex + count ||
          chart.texcoords.size() > draw.firstIndex + count - chart.firstIndex)
        throw invalid;
      for (size_t i = 0; i < chart.texcoords.size(); ++i) {
        const int64_t vertex = int64_t(surface.indices[chart.firstIndex + i]) + draw.vertexOffset;
        if (vertex < 0 || uint64_t(vertex) >= surface.vertices.size())
          throw invalid;
        for (uint32_t c = 0; c < 2; ++c) {
          const float uv = chart.texcoords[i][c];
          const float extent = float((c ? chart.height : chart.width) - 1);
          if (!std::isfinite(uv) || uv < 0 || uv > extent)
            throw invalid;
        }
      }
      validateMapping(chart, cancelled);
      ranges.push_back({chart.draw, chart.firstIndex, chart.firstIndex + chart.texcoords.size()});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) {
      return std::tie(a.draw, a.first) < std::tie(b.draw, b.first);
    });
    for (size_t i = 1; i < ranges.size(); ++i)
      if (ranges[i].draw == ranges[i - 1].draw && ranges[i].first < ranges[i - 1].end)
        throw invalid;
  }
} // namespace remix::scene
