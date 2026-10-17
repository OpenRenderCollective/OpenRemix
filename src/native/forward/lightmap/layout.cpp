#include "forward/lightmap/layout.h"
#include "forward/lightmap/packing.h"
#include "forward/lightmap/seams.h"
#include "forward/material.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace remix::forward::lightmap {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface lightmap planning superseded");
    }
    using Uv = std::array<float, 2>;
    double edge(Uv a, Uv b, double x, double y) {
      return (double(b[0]) - a[0]) * (y - a[1]) - (double(b[1]) - a[1]) * (x - a[0]);
    }
    // A fractional or coarsened face can cover a texel without containing its
    // centre. Clip to that texel's footprint and sample the covered centroid,
    // keeping rays on the real surface rather than extrapolating its plane.
    bool coveredPoint(const std::array<Uv, 3> &triangle, uint32_t x, uint32_t y, Uv &point) {
      std::vector<Uv> polygon(triangle.begin(), triangle.end()), clipped;
      for (uint32_t axis = 0; axis < 2; ++axis)
        for (float side : {-1.f, 1.f}) {
          if (polygon.empty())
            return false;
          const float boundary = float(axis == 0 ? x : y) + side * .5f;
          clipped.clear();
          auto previous = polygon.back();
          float previousDistance = side * (previous[axis] - boundary);
          for (auto current : polygon) {
            const float distance = side * (current[axis] - boundary);
            if ((distance <= 0) != (previousDistance <= 0)) {
              const float t = previousDistance / (previousDistance - distance);
              clipped.push_back({previous[0] + t * (current[0] - previous[0]),
                                 previous[1] + t * (current[1] - previous[1])});
            }
            if (distance <= 0)
              clipped.push_back(current);
            previous = current;
            previousDistance = distance;
          }
          polygon.swap(clipped);
        }
      double area = 0, weightedX = 0, weightedY = 0;
      for (size_t i = 1; i + 1 < polygon.size(); ++i) {
        const double weight = std::abs(edge(polygon[0], polygon[i], polygon[i + 1][0], polygon[i + 1][1]));
        area += weight;
        weightedX += weight * (double(polygon[0][0]) + polygon[i][0] + polygon[i + 1][0]) / 3;
        weightedY += weight * (double(polygon[0][1]) + polygon[i][1] + polygon[i + 1][1]) / 3;
      }
      if (area < 1e-12)
        return false;
      point = {float(weightedX / area), float(weightedY / area)};
      return true;
    }
    uint32_t address(const Layout &layout, const Allocation &chart, uint32_t x, uint32_t y) {
      return (chart.page * layout.pageSize + chart.y + y) * layout.pageSize + chart.x + x;
    }
    void rasterize(const scene::BakeScene &scene, uint32_t chartIndex, Layout &layout, const Options &options,
                   std::stop_token cancelled) {
      const auto &source = scene.charts[chartIndex];
      const auto &allocation = layout.charts[chartIndex];
      const auto &mapping = layout.mappings[chartIndex];
      const auto &draw = scene.draws[source.draw];
      const auto &surface = *draw.surface;
      const auto normal = normalMatrix(draw.model);
      const uint32_t paddedWidth = allocation.width + 2 * options.padding;
      const uint32_t paddedHeight = allocation.height + 2 * options.padding;
      std::vector<uint32_t> owners(size_t(paddedWidth) * paddedHeight, unowned);
      std::vector<float> interior(owners.size(), -1);
      std::vector<float> donorDistance(owners.size());
      std::vector<uint32_t> queue;
      double fallbackArea = 0;
      bake::Point fallbackPoint;
      uint32_t fallbackX = 0, fallbackY = 0;
      for (size_t triangle = 0; triangle < source.texcoords.size(); triangle += 3) {
        check(cancelled);
        std::array<Uv, 3> uv;
        std::array<Vec3, 3> positions, normals;
        for (uint32_t c = 0; c < 3; ++c) {
          uv[c] = mapping.texcoords[triangle + c];
          const auto index = int64_t(surface.indices[source.firstIndex + triangle + c]) + draw.vertexOffset;
          const auto &vertex = surface.vertices[size_t(index)];
          positions[c] =
              transformPoint(draw.model, {vertex.position[0], vertex.position[1], vertex.position[2]});
          normals[c] = transformVector(normal, {vertex.normal[0], vertex.normal[1], vertex.normal[2]});
        }
        const double area = edge(uv[0], uv[1], uv[2][0], uv[2][1]);
        if (std::abs(area) < 1e-12)
          continue;
        Vec3 a, b;
        for (uint32_t c = 0; c < 3; ++c) {
          a[c] = positions[1][c] - positions[0][c];
          b[c] = positions[2][c] - positions[0][c];
        }
        const auto geometric = cross(a, b);
        if (dot(geometric, geometric) < 1e-20f)
          continue;
        auto pointAt = [&](const std::array<double, 3> &weights) {
          bake::Point point;
          Vec3 n{};
          for (uint32_t c = 0; c < 3; ++c)
            for (uint32_t corner = 0; corner < 3; ++corner) {
              point.position[c] += positions[corner][c] * float(weights[corner]);
              n[c] += normals[corner][c] * float(weights[corner]);
            }
          if (dot(n, n) < 1e-12f)
            n = geometric;
          n = normalized(n);
          for (uint32_t c = 0; c < 3; ++c) {
            if (!std::isfinite(point.position[c]) || !std::isfinite(n[c]))
              throw std::invalid_argument("Lightmap surface transform is not finite");
            point.normal[c] = n[c];
          }
          return point;
        };
        if (std::abs(area) > fallbackArea) {
          fallbackArea = std::abs(area);
          fallbackPoint = pointAt({1. / 3, 1. / 3, 1. / 3});
          fallbackX =
              std::min(allocation.width - 1, uint32_t(std::lround((uv[0][0] + uv[1][0] + uv[2][0]) / 3)));
          fallbackY =
              std::min(allocation.height - 1, uint32_t(std::lround((uv[0][1] + uv[1][1] + uv[2][1]) / 3)));
        }
        const uint32_t x0 = uint32_t(std::max(0., std::ceil(std::min({uv[0][0], uv[1][0], uv[2][0]}) - .5)));
        const uint32_t x1 = std::min(allocation.width - 1,
                                     uint32_t(std::floor(std::max({uv[0][0], uv[1][0], uv[2][0]}) + .5)));
        const uint32_t y0 = uint32_t(std::max(0., std::ceil(std::min({uv[0][1], uv[1][1], uv[2][1]}) - .5)));
        const uint32_t y1 = std::min(allocation.height - 1,
                                     uint32_t(std::floor(std::max({uv[0][1], uv[1][1], uv[2][1]}) + .5)));
        if (x0 > x1 || y0 > y1)
          continue;
        for (uint32_t y = y0; y <= y1; ++y) {
          check(cancelled);
          for (uint32_t x = x0; x <= x1; ++x) {
            std::array<double, 3> weights = {edge(uv[1], uv[2], x, y) / area, edge(uv[2], uv[0], x, y) / area,
                                             edge(uv[0], uv[1], x, y) / area};
            const double minimum = *std::min_element(weights.begin(), weights.end());
            const bool centreInside = minimum >= -1e-7;
            Uv sample{float(x), float(y)};
            if (!centreInside) {
              if (!coveredPoint(uv, x, y, sample))
                continue;
              weights = {edge(uv[1], uv[2], sample[0], sample[1]) / area,
                         edge(uv[2], uv[0], sample[0], sample[1]) / area,
                         edge(uv[0], uv[1], sample[0], sample[1]) / area};
            }
            const float sampleDistance =
                (sample[0] - x) * (sample[0] - x) + (sample[1] - y) * (sample[1] - y);
            const auto point = pointAt(weights);
            const uint32_t local = (y + options.padding) * paddedWidth + x + options.padding;
            if (owners[local] != unowned) {
              const auto &old = layout.points[owners[local]];
              float distance = 0;
              for (uint32_t c = 0; c < 3; ++c) {
                const float delta = old.position[c] - point.position[c];
                distance += delta * delta;
              }
              const float epsilon = .0001f / scene.sceneScale;
              if (centreInside && interior[local] >= -1e-7f &&
                  ((minimum > 1e-6 && interior[local] > 1e-6f) || distance > epsilon * epsilon))
                throw std::invalid_argument("Overlapping or folded surface lightmap chart");
              if (interior[local] < -1e-7f && (centreInside || sampleDistance < donorDistance[local])) {
                layout.points[owners[local]] = point;
                layout.sampleCenters[owners[local]] = uint8_t(centreInside);
                interior[local] = centreInside ? float(minimum) : -1;
                donorDistance[local] = sampleDistance;
              }
              continue; // a covered texel retains one actual surface donor
            }
            owners[local] = uint32_t(layout.points.size());
            interior[local] = centreInside ? float(minimum) : -1;
            donorDistance[local] = sampleDistance;
            queue.push_back(local);
            layout.pointCharts.push_back(chartIndex);
            layout.points.push_back(point);
            layout.sampleCenters.push_back(uint8_t(centreInside));
            layout.receiverSamples.push_back(1);
            layout.addresses.push_back(address(layout, allocation, x, y));
          }
        }
      }
      if (queue.empty()) {
        if (fallbackArea == 0)
          throw std::invalid_argument("Surface lightmap chart has no nondegenerate surface");
        // A subtexel face may miss every integer luxel centre. Its donor
        // still belongs to the actual surface, never an extrapolated plane.
        const uint32_t local = (fallbackY + options.padding) * paddedWidth + fallbackX + options.padding;
        owners[local] = uint32_t(layout.points.size());
        queue.push_back(local);
        layout.pointCharts.push_back(chartIndex);
        layout.points.push_back(fallbackPoint);
        layout.sampleCenters.push_back(0);
        layout.receiverSamples.push_back(1);
        layout.addresses.push_back(address(layout, allocation, fallbackX, fallbackY));
      }
      // Only a one-texel ring around actual coverage can support the rendered
      // bilinear footprint. Unused interiors of a triangular chart's bounding
      // rectangle must not import the whole neighbouring face.
      std::vector<uint8_t> footprints(owners.size());
      for (auto pixel : queue) {
        const int x = int(pixel % paddedWidth), y = int(pixel / paddedWidth);
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
            if (x + dx >= 0 && y + dy >= 0 && x + dx < int(paddedWidth) && y + dy < int(paddedHeight))
              footprints[size_t(y + dy) * paddedWidth + size_t(x + dx)] = 1;
      }
      // Initial chart-local dilation. Once every chart has real surface samples,
      // seam completion can replace supported border taps with connected donors.
      for (size_t next = 0; next < queue.size(); ++next) {
        if ((next & 255u) == 0)
          check(cancelled);
        const auto pixel = queue[next];
        const uint32_t x = pixel % paddedWidth, y = pixel / paddedWidth;
        auto visit = [&](uint32_t neighbour) {
          if (owners[neighbour] == unowned) {
            owners[neighbour] = owners[pixel];
            queue.push_back(neighbour);
          }
        };
        if (x)
          visit(pixel - 1);
        if (x + 1 < paddedWidth)
          visit(pixel + 1);
        if (y)
          visit(pixel - paddedWidth);
        if (y + 1 < paddedHeight)
          visit(pixel + paddedWidth);
      }
      for (uint32_t y = 0; y < paddedHeight; ++y)
        for (uint32_t x = 0; x < paddedWidth; ++x) {
          const uint32_t atlas =
              (allocation.page * layout.pageSize + allocation.y - options.padding + y) * layout.pageSize +
              allocation.x - options.padding + x;
          layout.owners[atlas] = owners[y * paddedWidth + x];
          layout.footprints[atlas] = footprints[y * paddedWidth + x];
        }
    }
  } // namespace
  Layout plan(const scene::BakeScene &scene, Options options, std::stop_token cancelled) {
    const auto shapes = prepareCharts(scene, options, cancelled);
    auto result = packCharts(shapes, options, cancelled);
    return sampleCharts(scene, options, std::move(result), cancelled);
  }
  Layout sampleCharts(const scene::BakeScene &scene, const Options &options, Layout result,
                      std::stop_token cancelled) {
    result.owners.resize(size_t(result.pages) * options.pageSize * options.pageSize, unowned);
    result.footprints.resize(result.owners.size());
    for (uint32_t i = 0; i < scene.charts.size(); ++i)
      rasterize(scene, i, result, options, cancelled);
    extendSeams(scene, result, options.padding, cancelled);
    extendCurvedSeams(scene, result, options.padding, cancelled);
    return result;
  }
} // namespace remix::forward::lightmap
