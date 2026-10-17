#include "forward/lightmap/parameterization.h"
#include "forward/lighting.h"
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
    bool project(const scene::BakeScene &scene, const scene::BakeSurfaceChart &chart, double spacing,
                 Mapping &mapping, std::stop_token cancelled) {
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
      for (size_t i = 0; i < positions.size(); i += 3) {
        normal =
            cross3(difference(positions[i + 1], positions[i]), difference(positions[i + 2], positions[i]));
        if (dot3(normal, normal) > 1e-24)
          break;
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
      std::array<double, 2> lower{INFINITY, INFINITY}, upper{-INFINITY, -INFINITY};
      for (const auto &p : positions)
        for (uint32_t c = 0; c < 2; ++c) {
          const double coordinate = dot3(c ? mapping.v : mapping.u, p) / spacing;
          lower[c] = std::min(lower[c], coordinate);
          upper[c] = std::max(upper[c], coordinate);
        }
      uint32_t dimensions[2];
      for (uint32_t c = 0; c < 2; ++c) {
        const double first = std::floor(lower[c]), last = std::ceil(upper[c]);
        if (!std::isfinite(first) || !std::isfinite(last) || std::abs(first) > 1e12 ||
            std::abs(last) > 1e12 || last - first > double(std::numeric_limits<uint32_t>::max() - 1))
          throw std::length_error("Surface world grid exceeds coordinate limits");
        mapping.origin[c] = int64_t(first);
        dimensions[c] = uint32_t(std::max(2., last - first + 1));
      }
      mapping.width = dimensions[0];
      mapping.height = dimensions[1];
      mapping.texcoords.reserve(positions.size());
      for (const auto &p : positions)
        mapping.texcoords.push_back({float(dot3(mapping.u, p) / spacing - mapping.origin[0]),
                                     float(dot3(mapping.v, p) / spacing - mapping.origin[1])});
      mapping.worldGrid = true;
      return true;
    }
  } // namespace
  std::vector<Mapping> parameterize(const scene::BakeScene &scene, float luxelMetres, uint32_t step,
                                    std::stop_token cancelled) {
    std::vector<Mapping> result;
    result.reserve(scene.charts.size());
    for (const auto &chart : scene.charts) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface parameterization superseded");
      Mapping mapping;
      if (luxelMetres <= 0 || !project(scene, chart, double(luxelMetres) * step, mapping, cancelled)) {
        mapping = {};
        mapping.width = std::max(2u, 1 + (chart.width - 2 + step) / step);
        mapping.height = std::max(2u, 1 + (chart.height - 2 + step) / step);
        mapping.texcoords = chart.texcoords;
        for (auto &uv : mapping.texcoords) {
          uv[0] *= float(mapping.width - 1) / float(chart.width - 1);
          uv[1] *= float(mapping.height - 1) / float(chart.height - 1);
        }
      }
      result.push_back(std::move(mapping));
    }
    return result;
  }
} // namespace remix::forward::lightmap
