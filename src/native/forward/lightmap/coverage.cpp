#include "forward/lightmap/coverage.h"
#include "forward/lighting.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace {
    double edge(Coverage::Point a, Coverage::Point b, Coverage::Point p) {
      return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]);
    }
  } // namespace
  Coverage::Coverage(const scene::BakeScene &scene, const Layout &layout, std::span<const uint32_t> patches,
                     std::stop_token cancelled)
      : m_tolerance(.0001 / layout.luxelMetres) {
    for (uint32_t index = 0; index < scene.charts.size(); ++index) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Seam coverage planning superseded");
      const auto &mapping = layout.mappings[index];
      if (!mapping.worldGrid)
        continue;
      const auto &chart = scene.charts[index];
      const auto &draw = scene.draws[chart.draw];
      for (size_t first = 0; first < chart.texcoords.size(); first += 3) {
        if ((first & 1023u) == 0 && cancelled.stop_requested())
          throw std::runtime_error("Seam coverage planning superseded");
        Triangle triangle{};
        Point minimum{INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY};
        for (uint32_t c = 0; c < 3; ++c) {
          const auto vertex =
              int64_t(draw.surface->indices[chart.firstIndex + first + c]) + draw.vertexOffset;
          const auto &p = draw.surface->vertices[size_t(vertex)].position;
          const auto world = transformPoint(draw.model, {p[0], p[1], p[2]});
          auto &uv = triangle.points[c];
          for (uint32_t axis = 0; axis < 3; ++axis) {
            const double value = double(world[axis]) * scene.sceneScale / layout.luxelMetres;
            uv[0] += mapping.u[axis] * value;
            uv[1] += mapping.v[axis] * value;
          }
          for (uint32_t axis = 0; axis < 2; ++axis) {
            minimum[axis] = std::min(minimum[axis], uv[axis]);
            maximum[axis] = std::max(maximum[axis], uv[axis]);
          }
        }
        if (std::abs(edge(triangle.points[0], triangle.points[1], triangle.points[2])) < 1e-14)
          continue;
        const auto id = uint32_t(m_triangles.size());
        m_triangles.push_back(triangle);
        for (int64_t y = int64_t(std::floor((minimum[1] - m_tolerance) / cellSize));
             y <= int64_t(std::floor((maximum[1] + m_tolerance) / cellSize)); ++y)
          for (int64_t x = int64_t(std::floor((minimum[0] - m_tolerance) / cellSize));
               x <= int64_t(std::floor((maximum[0] + m_tolerance) / cellSize)); ++x)
            m_cells[{patches[index], x, y}].push_back(id);
      }
    }
  }
  bool Coverage::covers(uint32_t patch, Point start, Point end) {
    m_candidates.clear();
    m_intervals.clear();
    for (int64_t y = int64_t(std::floor((std::min(start[1], end[1]) - m_tolerance) / cellSize));
         y <= int64_t(std::floor((std::max(start[1], end[1]) + m_tolerance) / cellSize)); ++y)
      for (int64_t x = int64_t(std::floor((std::min(start[0], end[0]) - m_tolerance) / cellSize));
           x <= int64_t(std::floor((std::max(start[0], end[0]) + m_tolerance) / cellSize)); ++x) {
        const auto found = m_cells.find({patch, x, y});
        if (found != m_cells.end())
          m_candidates.insert(m_candidates.end(), found->second.begin(), found->second.end());
      }
    std::sort(m_candidates.begin(), m_candidates.end());
    m_candidates.erase(std::unique(m_candidates.begin(), m_candidates.end()), m_candidates.end());
    for (auto id : m_candidates) {
      const auto &triangle = m_triangles[id].points;
      const double sign = edge(triangle[0], triangle[1], triangle[2]) < 0 ? -1 : 1;
      double first = 0, last = 1;
      for (uint32_t c = 0; c < 3 && first <= last; ++c) {
        const auto a = triangle[c], b = triangle[(c + 1) % 3];
        const double tolerance = m_tolerance * std::hypot(b[0] - a[0], b[1] - a[1]);
        const double initial = sign * edge(a, b, start) + tolerance;
        const double delta = sign * edge(a, b, end) + tolerance - initial;
        if (std::abs(delta) < 1e-14) {
          if (initial < 0)
            first = 2;
        } else if (delta > 0)
          first = std::max(first, -initial / delta);
        else
          last = std::min(last, -initial / delta);
      }
      if (first <= 0 && last >= 1)
        return true;
      if (first <= last)
        m_intervals.push_back({first, last});
    }
    std::sort(m_intervals.begin(), m_intervals.end());
    double covered = 0;
    for (const auto &interval : m_intervals) {
      if (interval[0] > covered + 1e-8)
        return false;
      covered = std::max(covered, interval[1]);
      if (covered >= 1)
        return true;
    }
    return false;
  }
} // namespace remix::forward::lightmap
