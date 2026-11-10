#include "forward/lightmap/parameterization.h"
#include "forward/lighting.h"
#include "forward/lightmap/curved.h"
#include "forward/material.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace {
    using D3 = std::array<double, 3>;
    double dot3(D3 a, D3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    D3 cross3(D3 a, D3 b) {
      return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }
    D3 difference(D3 a, D3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
    D3 unit(D3 value) {
      const double length = std::sqrt(dot3(value, value));
      for (auto &c : value)
        c /= length;
      return value;
    }
    bool project(const scene::BakeScene &scene, const scene::BakeSurfaceChart &chart, ChartShape &shape,
                 std::stop_token cancelled) {
      auto &mapping = shape.frame;
      const auto &draw = scene.draws[chart.draw];
      std::vector<D3> positions;
      positions.reserve(chart.texcoords.size());
      for (size_t corner = 0; corner < chart.texcoords.size(); ++corner) {
        if ((corner & 1023u) == 0 && cancelled.stop_requested())
          throw std::runtime_error("Surface parameterization superseded");
        const auto index = int64_t(draw.surface->indices[chart.firstIndex + corner]) + draw.vertexOffset;
        const auto &p = draw.surface->vertices[size_t(index)].position;
        const auto world = transformPoint(draw.model, {p[0], p[1], p[2]});
        D3 metres;
        for (uint32_t c = 0; c < 3; ++c)
          metres[c] = double(world[c]) * scene.sceneScale;
        for (double value : metres)
          if (!std::isfinite(value))
            throw std::invalid_argument("Nonfinite lightmap world position");
        positions.push_back(metres);
      }
      D3 normal{};
      // Collinear BSP fan vertices can gain a tiny area through float rounding.
      // The largest triangle gives the face a stable plane instead of letting
      // the first such sliver select an arbitrary normal.
      for (size_t i = 0; i < positions.size(); i += 3) {
        const auto candidate =
            cross3(difference(positions[i + 1], positions[i]), difference(positions[i + 2], positions[i]));
        if (dot3(candidate, candidate) > dot3(normal, normal))
          normal = candidate;
      }
      if (dot3(normal, normal) <= 1e-24)
        return false;
      const auto normals = normalMatrix(draw.model);
      const auto first = int64_t(draw.surface->indices[chart.firstIndex]) + draw.vertexOffset;
      const auto &n = draw.surface->vertices[size_t(first)].normal;
      const auto outward = transformVector(normals, {n[0], n[1], n[2]});
      if (dot3(normal, {outward[0], outward[1], outward[2]}) < 0)
        for (auto &c : normal)
          c = -c;
      normal = unit(normal);
      const double plane = dot3(normal, positions.front());
      for (const auto &p : positions)
        if (std::abs(dot3(normal, p) - plane) > .0001)
          return false;
      // Quantizing only the orientation gives neighbouring faces the same basis.
      // Sample positions are still interpolated on real geometry, not this plane.
      for (uint32_t c = 0; c < 3; ++c) {
        mapping.normalKey[c] = std::llround(normal[c] * 1000000);
        normal[c] = double(mapping.normalKey[c]);
      }
      normal = unit(normal);
      const D3 reference = std::abs(normal[0]) < .8 ? D3{1, 0, 0} : D3{0, 1, 0};
      mapping.u = unit(cross3(reference, normal));
      mapping.v = cross3(normal, mapping.u);
      shape.coordinates.reserve(positions.size());
      for (const auto &p : positions)
        shape.coordinates.push_back({dot3(mapping.u, p), dot3(mapping.v, p)});
      mapping.worldGrid = true;
      return true;
    }
  } // namespace
  std::vector<ChartShape> prepareShapes(const scene::BakeScene &scene, bool worldMetric,
                                        std::stop_token cancelled) {
    std::vector<ChartShape> result;
    result.reserve(scene.charts.size());
    for (const auto &chart : scene.charts) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface parameterization superseded");
      ChartShape shape;
      if (worldMetric) {
        if (!project(scene, chart, shape, cancelled)) {
          shape = {};
          auto curved = curvedCoordinates(scene, chart, cancelled);
          shape.frame.curvedMetric = true;
          shape.frame.intrinsic = curved.intrinsic;
          shape.coordinates = std::move(curved.metres);
        }
      } else {
        shape.frame.width = chart.width;
        shape.frame.height = chart.height;
        for (const auto &uv : chart.texcoords)
          shape.coordinates.push_back({uv[0], uv[1]});
      }
      result.push_back(std::move(shape));
    }
    return result;
  }
  std::vector<Mapping> parameterize(std::span<const ChartShape> shapes, float luxelMetres, uint32_t step,
                                    std::stop_token cancelled) {
    std::vector<Mapping> result;
    result.reserve(shapes.size());
    for (const auto &shape : shapes) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface parameterization superseded");
      Mapping mapping = shape.frame;
      if (luxelMetres <= 0) {
        mapping.width = std::max(2u, 1 + (shape.frame.width - 2 + step) / step);
        mapping.height = std::max(2u, 1 + (shape.frame.height - 2 + step) / step);
        for (const auto &uv : shape.coordinates)
          mapping.texcoords.push_back({float(uv[0] * double(mapping.width - 1) / (shape.frame.width - 1)),
                                       float(uv[1] * double(mapping.height - 1) / (shape.frame.height - 1))});
      } else {
        const double spacing = double(luxelMetres) * step;
        std::array<double, 2> lower{INFINITY, INFINITY}, upper{-INFINITY, -INFINITY};
        for (const auto &uv : shape.coordinates)
          for (uint32_t c = 0; c < 2; ++c) {
            lower[c] = std::min(lower[c], uv[c] / spacing);
            upper[c] = std::max(upper[c], uv[c] / spacing);
          }
        uint32_t dimensions[2];
        for (uint32_t c = 0; c < 2; ++c) {
          const double first = std::floor(lower[c]), last = std::ceil(upper[c]);
          if (!std::isfinite(first) || !std::isfinite(last) || std::abs(first) > 1e12 ||
              std::abs(last) > 1e12 || last - first > double(std::numeric_limits<uint32_t>::max() - 1))
            throw std::length_error("Surface grid " + std::to_string(result.size()) + " range " +
                                    std::to_string(first) + ".." + std::to_string(last) + " exceeds limits");
          mapping.origin[c] = int64_t(first);
          dimensions[c] = uint32_t(std::max(2., last - first + 1));
        }
        mapping.width = dimensions[0];
        mapping.height = dimensions[1];
        for (const auto &uv : shape.coordinates)
          mapping.texcoords.push_back(
              {float(uv[0] / spacing - mapping.origin[0]), float(uv[1] / spacing - mapping.origin[1])});
      }
      result.push_back(std::move(mapping));
    }
    return result;
  }
} // namespace remix::forward::lightmap
