#include "forward/bake/model_charts.h"
#include "forward/bake/probe_receiver_geometry.h"
#include "forward/material.h"
#include "forward/lightmap/curved.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    std::string exhausted(const scene::BakeScene &source,
                          const std::vector<scene::BakeSurfaceChart> &charts, uint32_t draw, uint32_t first) {
      struct Use { uint32_t draw = 0, charts = 0; uint64_t triangles = 0; };
      std::map<uint32_t, Use> uses;
      for (const auto &chart : charts) {
        auto &use = uses[chart.draw];
        use.draw = chart.draw; ++use.charts; use.triangles += chart.texcoords.size() / 3;
      }
      std::vector<Use> ranked;
      for (const auto &[index, use] : uses) ranked.push_back(use);
      std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.charts != b.charts ? a.charts > b.charts : a.draw < b.draw;
      });
      std::ostringstream out;
      out << "Model chart budget exhausted: " << charts.size() << " charts across " << uses.size()
          << " draws; stopped at draw " << draw << " index " << first << "; largest prefix consumers:";
      for (size_t i = 0; i < std::min<size_t>(8, ranked.size()); ++i) {
        const auto &use = ranked[i];
        const auto &owner = source.draws[use.draw];
        out << " [draw " << use.draw << ", charts " << use.charts << ", accepted triangles " << use.triangles
            << ", draw triangles " << (owner.indexCount ? owner.indexCount : owner.surface->indices.size()) / 3
            << ", material " << std::hex << (owner.material ? owner.material->sourceHash : 0) << std::dec << ']';
      }
      return out.str();
    }
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
      for (size_t first = 3; first < points.size(); first += 3) {
        std::array<double, 3> n;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const uint32_t a = (axis + 1) % 3, b = (axis + 2) % 3;
          n[axis] = (double(points[first + 1][a]) - points[first][a]) * (double(points[first + 2][b]) - points[first][b]) -
                    (double(points[first + 1][b]) - points[first][b]) * (double(points[first + 2][a]) - points[first][a]);
        }
        const double dot = n[0] * normal[0] + n[1] * normal[1] + n[2] * normal[2];
        if (dot <= .5 * std::hypot(n[0], n[1], n[2])) return false;
      }
      uint32_t drop = 0;
      for (uint32_t axis = 1; axis < 3; ++axis)
        if (std::abs(normal[axis]) > std::abs(normal[drop])) drop = axis;
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
        try { lightmap::curvedCoordinates(source, merged, cancelled); }
        catch (const std::invalid_argument &) { return false; }
      }
      chart = std::move(merged);
      return true;
    }
  }
  std::vector<scene::BakeSurfaceChart> modelCharts(const scene::BakeScene &source,
      uint32_t maximumCharts, std::stop_token cancelled) {
    if (!maximumCharts || maximumCharts > 65536)
      throw std::invalid_argument("Model chart budget is 1..65536");
    if (!std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid model chart scene scale");
    const auto ranges = unchartedReceiverRanges(source, cancelled);
    std::vector<scene::BakeSurfaceChart> result;
    for (const auto &range : ranges) {
      const auto &draw = source.draws[range.draw];
      if (draw.classification != REMIXAPI_INSTANCE_STATIC_EXT) continue;
      for (size_t first = range.firstIndex; first < size_t(range.firstIndex) + range.indexCount; first += 3) {
        if (cancelled.stop_requested()) throw std::runtime_error("Model chart generation superseded");
        std::array<Vec3, 3> positions;
        for (uint32_t corner = 0; corner < 3; ++corner) {
          const int64_t index = int64_t(draw.surface->indices[first + corner]) + draw.vertexOffset;
          if (index < 0 || uint64_t(index) >= draw.surface->vertices.size())
            throw std::invalid_argument("Invalid model chart vertex index");
          const auto &vertex = draw.surface->vertices[size_t(index)];
          positions[corner] = transformPoint(draw.model, {vertex.position[0], vertex.position[1], vertex.position[2]});
          for (float value : positions[corner])
            if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite model chart position");
        }
        std::array<double, 3> a, b, cross;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          a[axis] = double(positions[1][axis]) - positions[0][axis];
          b[axis] = double(positions[2][axis]) - positions[0][axis];
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
          cross[axis] = a[(axis + 1) % 3] * b[(axis + 2) % 3] - a[(axis + 2) % 3] * b[(axis + 1) % 3];
        // Match the world-metric parameterizer's degeneracy threshold.
        const double area = std::hypot(cross[0], cross[1], cross[2]) *
                            double(source.sceneScale) * source.sceneScale;
        const double edges = std::hypot(a[0], a[1], a[2]) * std::hypot(b[0], b[1], b[2]);
        if (!(area > 1e-12) || std::hypot(cross[0], cross[1], cross[2]) <= edges * 1e-6) continue;
        if (!playableReceiverTriangle(source, positions, cancelled)) continue;
        scene::BakeSurfaceChart chart{range.draw, uint32_t(first), 2, 2, {{0, 0}, {1, 0}, {0, 1}}};
        if (!result.empty() && join(source, result.back(), chart, cancelled)) continue;
        if (result.size() == maximumCharts) throw std::length_error(exhausted(source, result, range.draw, uint32_t(first)));
        result.push_back(std::move(chart));
      }
    }
    return result;
  }
}
