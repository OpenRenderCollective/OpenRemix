#include "forward/probes/receivers.h"
#include "forward/material.h"
#include <cmath>
#include <stdexcept>

namespace remix::forward::probes {
  namespace {
    using Point = std::array<double, 3>;
    struct Triangle {
      std::array<Point, 3> positions, normals;
      Point facing;
      double area = 0;
      uint32_t draw = 0, chart = 0, firstIndex = 0;
    };
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection receiver sampling superseded");
    }
    Point unit(Point value) {
      const double length = std::hypot(value[0], value[1], value[2]);
      if (length > 0)
        for (auto &component : value) component /= length;
      return value;
    }
    template <class Visitor>
    void visit(const scene::BakeScene &source, std::stop_token cancelled, Visitor visitor) {
      for (uint32_t chartIndex = 0; chartIndex < source.charts.size(); ++chartIndex) {
        const auto &chart = source.charts[chartIndex];
        const auto &draw = source.draws.at(chart.draw);
        if (!draw.surface || chart.texcoords.size() % 3)
          throw std::invalid_argument("Invalid reflection receiver chart");
        const auto &surface = *draw.surface;
        const auto transform = normalMatrix(draw.model);
        for (size_t first = 0; first < chart.texcoords.size(); first += 3) {
          check(cancelled);
          Triangle triangle;
          triangle.draw = chart.draw;
          triangle.chart = chartIndex;
          triangle.firstIndex = chart.firstIndex + uint32_t(first);
          for (uint32_t corner = 0; corner < 3; ++corner) {
            const int64_t index = int64_t(surface.indices.at(size_t(chart.firstIndex) + first + corner))
                                  + draw.vertexOffset;
            if (index < 0 || uint64_t(index) >= surface.vertices.size())
              throw std::invalid_argument("Invalid reflection receiver vertex");
            const auto &vertex = surface.vertices[size_t(index)];
            const auto position = transformPoint(draw.model,
                {vertex.position[0], vertex.position[1], vertex.position[2]});
            const auto normal = transformVector(transform,
                {vertex.normal[0], vertex.normal[1], vertex.normal[2]});
            for (uint32_t axis = 0; axis < 3; ++axis) {
              if (!std::isfinite(position[axis]) || !std::isfinite(normal[axis]))
                throw std::invalid_argument("Non-finite reflection receiver geometry");
              triangle.positions[corner][axis] = position[axis];
              triangle.normals[corner][axis] = normal[axis];
            }
          }
          Point a, b;
          for (uint32_t axis = 0; axis < 3; ++axis) {
            a[axis] = triangle.positions[1][axis] - triangle.positions[0][axis];
            b[axis] = triangle.positions[2][axis] - triangle.positions[0][axis];
          }
          triangle.facing = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                             a[0] * b[1] - a[1] * b[0]};
          triangle.area = .5 * std::hypot(triangle.facing[0], triangle.facing[1], triangle.facing[2]) *
                          double(source.sceneScale) * source.sceneScale;
          triangle.facing = unit(triangle.facing);
          if (triangle.area > 0) visitor(triangle);
        }
      }
    }
    double fraction(double value) { return value - std::floor(value); }
  } // namespace
  std::vector<ReceiverSample> sampleReceivers(const scene::BakeScene &source, uint32_t count,
                                              std::stop_token cancelled) {
    check(cancelled);
    if (!count || count > 65536 || !std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid reflection receiver sample budget or scale");
    double total = 0;
    visit(source, cancelled, [&](const Triangle &triangle) { total += triangle.area; });
    if (!std::isfinite(total))
      throw std::invalid_argument("Non-finite reflection receiver area");
    if (!total) return {};
    std::vector<ReceiverSample> result;
    result.reserve(count);
    double cumulative = 0;
    visit(source, cancelled, [&](const Triangle &triangle) {
      cumulative += triangle.area;
      while (result.size() < count && (double(result.size()) + .5) * total / count < cumulative) {
        // Stratify surface area globally; use two independent irrational
        // rotations inside each chosen triangle to avoid repeated centroids.
        const double ordinal = double(result.size()) + .5;
        const double a = std::sqrt(fraction(ordinal * .7548776662466927));
        const double b = fraction(ordinal * .5698402909980532);
        const std::array<double, 3> weights{1 - a, a * (1 - b), a * b};
        ReceiverSample sample;
        sample.draw = triangle.draw;
        sample.chart = triangle.chart;
        sample.firstIndex = triangle.firstIndex;
        sample.areaMetresSquared = total / count;
        Point normal{};
        for (uint32_t corner = 0; corner < 3; ++corner)
          for (uint32_t axis = 0; axis < 3; ++axis) {
            sample.position[axis] += float(triangle.positions[corner][axis] * weights[corner]);
            normal[axis] += triangle.normals[corner][axis] * weights[corner];
          }
        normal = unit(normal);
        if (normal == Point{}) normal = triangle.facing;
        for (uint32_t axis = 0; axis < 3; ++axis) sample.normal[axis] = float(normal[axis]);
        result.push_back(sample);
      }
    });
    return result;
  }
} // namespace remix::forward::probes
