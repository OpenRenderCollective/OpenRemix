#include "forward/bake/model_charts.h"
#include "forward/bake/model_chart_report.h"
#include "forward/bake/model_triangles.h"
#include "forward/material.h"
#include "forward/lightmap/curved.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    // Keep charts bounded, contiguous and within one smooth connected patch.
    // Projection must remain injective; the existing chart validator rejects
    // folds/overlaps before a merge can replace its original triangles.
    bool join(const scene::BakeScene &source, scene::BakeSurfaceChart &chart,
              const scene::BakeSurfaceChart &next, std::stop_token cancelled) {
      if (chart.draw != next.draw || chart.firstIndex + chart.texcoords.size() != next.firstIndex ||
          chart.texcoords.size() >= 192) return false;
      const auto &draw = source.draws[chart.draw];
      const auto &indices = draw.surface->indices;
      uint32_t shared = 0;
      for (uint32_t corner = 0; corner < 3; ++corner) {
        const auto index = indices[next.firstIndex + corner];
        const auto first = indices.begin() + chart.firstIndex;
        if (std::find(first, first + chart.texcoords.size(), index) != first + chart.texcoords.size()) ++shared;
      }
      if (shared < 2) return false;
      auto merged = chart;
      merged.texcoords.resize(chart.texcoords.size() + 3);
      std::vector<Vec3> points;
      for (size_t corner = 0; corner < merged.texcoords.size(); ++corner) {
        const auto index = int64_t(indices[chart.firstIndex + corner]) + draw.vertexOffset;
        const auto &p = draw.surface->vertices[size_t(index)].position;
        points.push_back(transformPoint(draw.model, {p[0], p[1], p[2]}));
      }
      std::array<double, 3> normal{};
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const uint32_t a = (axis + 1) % 3, b = (axis + 2) % 3;
        normal[axis] = (double(points[1][a]) - points[0][a]) * (double(points[2][b]) - points[0][b]) -
                       (double(points[1][b]) - points[0][b]) * (double(points[2][a]) - points[0][a]);
      }
      const double length = std::hypot(normal[0], normal[1], normal[2]);
      for (auto &value : normal) value /= length;
      uint32_t drop = 0;
      for (uint32_t axis = 1; axis < 3; ++axis)
        if (std::abs(normal[axis]) > std::abs(normal[drop])) drop = axis;
      for (size_t first = 3; first < points.size(); first += 3) {
        std::array<double, 3> n;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const uint32_t a = (axis + 1) % 3, b = (axis + 2) % 3;
          n[axis] = (double(points[first + 1][a]) - points[first][a]) * (double(points[first + 2][b]) - points[first][b]) -
                    (double(points[first + 1][b]) - points[first][b]) * (double(points[first + 2][a]) - points[first][a]);
        }
        const double dot = n[0] * normal[0] + n[1] * normal[1] + n[2] * normal[2];
        const double magnitude = std::hypot(n[0], n[1], n[2]);
        if (dot <= .5 * magnitude) return false;
        // The dropped-axis projection must not view a face at grazing angles.
        // Otherwise metric correction can inflate a tiny patch by kilometres.
        const double projected = normal[drop] < 0 ? -n[drop] : n[drop];
        if (projected < .5 * magnitude) return false;
      }

      for (uint32_t uv = 0; uv < 2; ++uv) {
        const uint32_t axis = (drop + 1 + uv) % 3;
        double minimum = INFINITY, maximum = -INFINITY;
        for (const auto &p : points) {
          minimum = std::min(minimum, double(p[axis]));
          maximum = std::max(maximum, double(p[axis]));
        }
        if (!(maximum > minimum)) return false;
        for (size_t i = 0; i < points.size(); ++i)
          merged.texcoords[i][uv] = float((double(points[i][axis]) - minimum) / (maximum - minimum));
      }
      try { scene::validateBakeCharts(source.draws, {merged}, cancelled); }
      catch (remixapi_ErrorCode error) {
        if (error != REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS) throw;
        return false;
      }
      bool planar = true;
      for (const auto &p : points) {
        double distance = 0;
        for (uint32_t axis = 0; axis < 3; ++axis)
          distance += normal[axis] * (double(p[axis]) - points[0][axis]) * source.sceneScale;
        if (std::abs(distance) > .0001) planar = false;
      }
      if (!planar) {
        try {
          const auto curved = lightmap::curvedCoordinates(source, merged, cancelled);
          std::array<double, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
          for (const auto &p : points)
            for (uint32_t axis = 0; axis < 3; ++axis) {
              low[axis] = std::min(low[axis], double(p[axis]));
              high[axis] = std::max(high[axis], double(p[axis]));
            }
          const double diameter = std::hypot(high[0] - low[0], high[1] - low[1], high[2] - low[2]) * source.sceneScale;
          std::array<double, 2> uvLow{INFINITY, INFINITY}, uvHigh{-INFINITY, -INFINITY};
          for (const auto &p : curved.metres)
            for (uint32_t axis = 0; axis < 2; ++axis) {
              uvLow[axis] = std::min(uvLow[axis], p[axis]);
              uvHigh[axis] = std::max(uvHigh[axis], p[axis]);
            }
          // Split ill-conditioned generated patches rather than sacrificing
          // global atlas density. Authored charts are never subject to this rule.
          if (std::max(uvHigh[0] - uvLow[0], uvHigh[1] - uvLow[1]) > 4 * diameter + 1e-6) return false;
        }
        catch (const std::invalid_argument &) { return false; }
      }
      chart = std::move(merged);
      return true;
    }
  }
  std::vector<scene::BakeSurfaceChart> modelCharts(const scene::BakeScene &source,
      uint32_t maximumCharts, std::stop_token cancelled) {
    if (!maximumCharts || maximumCharts > 1048576)
      throw std::invalid_argument("Model chart budget is 1..1048576");
    std::vector<scene::BakeSurfaceChart> result;
    forEachModelTriangle(source, [&](const ModelTriangle &triangle) {
      scene::BakeSurfaceChart chart{triangle.draw, triangle.firstIndex, 2, 2, {{0, 0}, {1, 0}, {0, 1}}};
      if (!result.empty() && join(source, result.back(), chart, cancelled)) return;
      if (result.size() == maximumCharts)
        throw std::length_error(modelChartBudgetFailure(source, result, triangle.draw, triangle.firstIndex, cancelled));
      result.push_back(std::move(chart));
    }, cancelled);
    return result;
  }
}
