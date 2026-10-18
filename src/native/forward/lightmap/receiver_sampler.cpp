#include "forward/lightmap/receiver_sampler.h"
#include "forward/material.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace {
    Vec3 difference(Vec3 a, Vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
    double edge(ReceiverFootprint::Uv a, ReceiverFootprint::Uv b, ReceiverFootprint::Uv p) {
      return (double(b[0]) - a[0]) * (double(p[1]) - a[1]) - (double(b[1]) - a[1]) * (double(p[0]) - a[0]);
    }
  } // namespace
  ReceiverSampler::ReceiverSampler(const scene::BakeScene &source, const Layout &layout)
      : m_source(source), m_layout(layout) {
    if (source.charts.size() != layout.mappings.size() || source.charts.size() != layout.charts.size() ||
        layout.points.size() != layout.pointCharts.size() ||
        layout.points.size() != layout.addresses.size() ||
        (!layout.receiverSamples.empty() && layout.receiverSamples.size() != layout.points.size()) ||
        (!layout.points.empty() && !layout.pageSize))
      throw std::invalid_argument("Incomplete receiver sampling geometry");
    m_hits.reserve(8);
  }
  void ReceiverSampler::prepare(uint32_t chartIndex) {
    m_triangles.clear();
    const auto &chart = m_source.charts.at(chartIndex);
    const auto &mapping = m_layout.mappings.at(chartIndex);
    const auto &draw = m_source.draws.at(chart.draw);
    const auto &surface = *draw.surface;
    if (mapping.texcoords.size() % 3 || mapping.texcoords.size() != chart.texcoords.size())
      throw std::invalid_argument("Incomplete receiver chart triangles");
    const auto normal = normalMatrix(draw.model);
    for (size_t first = 0; first < mapping.texcoords.size(); first += 3) {
      Triangle triangle;
      for (uint32_t corner = 0; corner < 3; ++corner) {
        triangle.uv[corner] = mapping.texcoords[first + corner];
        const auto index = int64_t(surface.indices.at(chart.firstIndex + first + corner)) + draw.vertexOffset;
        const auto &vertex = surface.vertices.at(size_t(index));
        triangle.positions[corner] =
            transformPoint(draw.model, {vertex.position[0], vertex.position[1], vertex.position[2]});
        triangle.normals[corner] =
            transformVector(normal, {vertex.normal[0], vertex.normal[1], vertex.normal[2]});
      }
      triangle.area = edge(triangle.uv[0], triangle.uv[1], triangle.uv[2]);
      const auto geometric = cross(difference(triangle.positions[1], triangle.positions[0]),
                                   difference(triangle.positions[2], triangle.positions[0]));
      if (std::abs(triangle.area) < 1e-12 || dot(geometric, geometric) < 1e-20f)
        continue;
      for (uint32_t axis = 0; axis < 2; ++axis) {
        triangle.minimum[axis] = std::min({triangle.uv[0][axis], triangle.uv[1][axis], triangle.uv[2][axis]});
        triangle.maximum[axis] = std::max({triangle.uv[0][axis], triangle.uv[1][axis], triangle.uv[2][axis]});
      }
      m_triangles.push_back(triangle);
    }
    m_chart = chartIndex;
  }
  std::array<bake::Point, 4> ReceiverSampler::sample(uint32_t index) {
    const auto &centre = m_layout.points.at(index);
    std::array<bake::Point, 4> result{centre, centre, centre, centre};
    // Verified seam extension donors already belong to neighbouring geometry.
    // Preserve their actual origins rather than remapping through another face.
    if (!m_layout.receiverSamples.empty() && !m_layout.receiverSamples[index])
      return result;
    const auto chartIndex = m_layout.pointCharts[index];
    if (chartIndex != m_chart)
      prepare(chartIndex);
    const auto &allocation = m_layout.charts.at(chartIndex);
    const auto address = m_layout.addresses[index];
    const auto x = int64_t(address % m_layout.pageSize) - allocation.x;
    const auto y = int64_t((address / m_layout.pageSize) % m_layout.pageSize) - allocation.y;
    if (x < 0 || y < 0 || x >= allocation.width || y >= allocation.height)
      throw std::invalid_argument("Receiver address leaves its chart");
    const ReceiverFootprint::Uv uv{float(x), float(y)};
    m_hits.clear();
    double totalArea = 0;
    for (uint32_t i = 0; i < m_triangles.size(); ++i) {
      const auto &triangle = m_triangles[i];
      if (triangle.minimum[0] > uv[0] + .5f || triangle.maximum[0] < uv[0] - .5f ||
          triangle.minimum[1] > uv[1] + .5f || triangle.maximum[1] < uv[1] - .5f)
        continue;
      ReceiverFootprint footprint(triangle.uv, uv);
      if (footprint.empty())
        continue;
      totalArea += footprint.area();
      m_hits.push_back({i, footprint});
    }
    if (m_hits.empty()) {
      ++m_fallbackCount;
      return result; // subtexel donor retains its actual surface origin
    }
    // Stable receiver-only quadrature: changing lights, chunks or frame slots
    // cannot move these sites. All covering triangles participate by area.
    uint32_t seed = address * 19349663u ^ chartIndex * 73856093u;
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    const double shiftU = double(seed & 65535u) / 65536.;
    const double shiftV = double(seed >> 16) / 65536.;
    constexpr double strataV[] = {.125, .625, .375, .875};
    for (uint32_t site = 0; site < 4; ++site) {
      double target = (site + shiftU) * .25 * totalArea;
      const Hit *selected = &m_hits.back();
      for (const auto &hit : m_hits) {
        if (target < hit.footprint.area() || &hit == &m_hits.back()) {
          selected = &hit;
          break;
        }
        target -= hit.footprint.area();
      }
      const auto point = selected->footprint.sample(
          std::clamp(target / selected->footprint.area(), 0., std::nextafter(1., 0.)),
          std::fmod(strataV[site] + shiftV, 1.));
      const auto &triangle = m_triangles[selected->triangle];
      const double weights[] = {edge(triangle.uv[1], triangle.uv[2], point) / triangle.area,
                                edge(triangle.uv[2], triangle.uv[0], point) / triangle.area,
                                edge(triangle.uv[0], triangle.uv[1], point) / triangle.area};
      result[site] = {};
      Vec3 n{};
      for (uint32_t axis = 0; axis < 3; ++axis)
        for (uint32_t corner = 0; corner < 3; ++corner) {
          result[site].position[axis] += triangle.positions[corner][axis] * float(weights[corner]);
          n[axis] += triangle.normals[corner][axis] * float(weights[corner]);
        }
      if (dot(n, n) < 1e-12f)
        n = cross(difference(triangle.positions[1], triangle.positions[0]),
                  difference(triangle.positions[2], triangle.positions[0]));
      n = normalized(n);
      std::copy(n.begin(), n.end(), result[site].normal.begin());
    }
    return result;
  }
  uint64_t ReceiverSampler::memoryBytes() const {
    return m_triangles.capacity() * sizeof(Triangle) + m_hits.capacity() * sizeof(Hit);
  }
} // namespace remix::forward::lightmap
