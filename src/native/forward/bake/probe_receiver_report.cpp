#include "forward/bake/probe_receiver_report.h"
#include "forward/bake/spherical_harmonics.h"
#include "forward/material.h"
#include "scene/bake_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>

namespace remix::forward::bake {
  namespace {
    using Double3 = std::array<double, 3>;
    Double3 difference(Vec3 a, Vec3 b) { return {double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]}; }
    Double3 crossDouble(Double3 a, Double3 b) {
      return {a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]};
    }
    double dotDouble(Double3 a, Double3 b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Probe receiver report superseded");
    }
    template<size_t N> void vector(std::ostream &out, const std::array<float, N> &value) {
      out << '[';
      for (size_t i = 0; i < N; ++i) { if (i) out << ','; out << value[i]; }
      out << ']';
    }
    std::vector<Grid::Origin> probeOrigins(const Grid &grid) {
      if (indexed(grid.header)) return directoryOrigins(grid);
      const uint32_t count = gridNodes(grid.header);
      if (grid.probes.size() != count || grid.header.dimensions[3] != count)
        throw std::invalid_argument("Invalid dense probe receiver grid");
      std::vector<Grid::Origin> result(count);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t coordinate = i;
        for (uint32_t c = 0; c < 3; ++c) {
          result[i].position[c] = grid.header.originSpacing[c] +
              (coordinate % grid.header.dimensions[c]) * grid.header.originSpacing[3];
          coordinate /= grid.header.dimensions[c];
        }
        result[i].spacing = grid.header.originSpacing[3];
      }
      return result;
    }
  }

  std::vector<std::optional<ReceiverHit>> pickProbeReceivers(const scene::BakeScene &source,
      std::span<const ReceiverRay> rays, std::stop_token cancelled) {
    check(cancelled);
    if (rays.empty() || rays.size() > 16) throw std::invalid_argument("Probe receiver ray budget is 1..16");
    for (const auto &ray : rays) {
      for (uint32_t c = 0; c < 3; ++c)
        if (!std::isfinite(ray.origin[c]) || !std::isfinite(ray.direction[c]))
          throw std::invalid_argument("Nonfinite probe receiver ray");
      if (std::abs(dot(ray.direction, ray.direction) - 1) > 1e-4f)
        throw std::invalid_argument("Probe receiver ray direction must be unit length");
    }
    std::vector<std::optional<ReceiverHit>> hits(rays.size());
    std::vector<double> closest(rays.size(), std::numeric_limits<double>::infinity());
    for (uint32_t drawIndex = 0; drawIndex < source.draws.size(); ++drawIndex) {
      check(cancelled);
      const auto &draw = source.draws[drawIndex];
      if (!draw.surface || !draw.traced() || draw.decal || draw.viewModel || draw.reflectionOnly || draw.wireframe ||
          draw.bones || draw.blend.enabled ||
          (draw.material && (draw.material->transmission || draw.material->translucent))) continue;
      const auto &surface = *draw.surface;
      const uint32_t compare = draw.alphaOverride || (draw.material && draw.material->drawAlpha)
          ? draw.compare : draw.material ? draw.material->compare : 7;
      if (!draw.ignoreAlpha && compare != 7) continue; // No claim about alpha texel coverage.
      const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
      if (draw.firstIndex > surface.indices.size() || count > surface.indices.size() - draw.firstIndex || count % 3)
        throw std::invalid_argument("Invalid probe receiver draw range");
      const auto normalTransform = normalMatrix(draw.model);
      for (size_t first = draw.firstIndex; first < draw.firstIndex + count; first += 3) {
        if (first % 1024 < 3) check(cancelled);
        std::array<const scene::Vertex *, 3> vertices;
        std::array<Vec3, 3> positions;
        for (uint32_t c = 0; c < 3; ++c) {
          const int64_t index = int64_t(surface.indices[first + c]) + draw.vertexOffset;
          vertices[c] = &surface.vertices.at(size_t(index));
          const auto &v = *vertices[c];
          positions[c] = transformPoint(draw.model, {v.position[0], v.position[1], v.position[2]});
        }
        const auto edge1 = difference(positions[1], positions[0]), edge2 = difference(positions[2], positions[0]);
        const auto geometric = crossDouble(edge1, edge2);
        const double area = std::sqrt(dotDouble(geometric, geometric));
        if (!(area > 0) || !std::isfinite(area)) continue;
        for (size_t r = 0; r < rays.size(); ++r) {
          const auto &ray = rays[r];
          const Double3 direction{ray.direction[0], ray.direction[1], ray.direction[2]};
          const auto p = crossDouble(direction, edge2);
          const double determinant = dotDouble(edge1, p);
          if (std::abs(determinant) <= area * 1e-12) continue;
          const auto offset = difference(ray.origin, positions[0]);
          const double u = dotDouble(offset, p) / determinant;
          if (u < 0 || u > 1) continue;
          const auto q = crossDouble(offset, edge1);
          const double v = dotDouble(direction, q) / determinant;
          if (v < 0 || u + v > 1) continue;
          const double distance = dotDouble(edge2, q) / determinant;
          if (!(distance > 0) || !std::isfinite(distance) || distance > std::numeric_limits<float>::max() ||
              distance >= closest[r]) continue;
          ReceiverHit hit;
          hit.draw = drawIndex; hit.firstIndex = uint32_t(first); hit.distance = float(distance);
          for (uint32_t c = 0; c < 3; ++c) {
            hit.position[c] = float(positions[0][c] * (1 - u - v) + positions[1][c] * u + positions[2][c] * v);
            hit.normal[c] = float(vertices[0]->normal[c] * (1 - u - v) + vertices[1]->normal[c] * u + vertices[2]->normal[c] * v);
            hit.geometricNormal[c] = float(geometric[c] / area);
          }
          hit.normal = transformVector(normalTransform, hit.normal);
          hit.normal = dot(hit.normal, hit.normal) > 1e-20f ? normalized(hit.normal) : hit.geometricNormal;
          if (dot(hit.normal, ray.direction) > 0) for (auto &c : hit.normal) c = -c;
          if (dot(hit.geometricNormal, ray.direction) > 0) for (auto &c : hit.geometricNormal) c = -c;
          hits[r] = hit;
          closest[r] = distance;
        }
      }
    }
    return hits;
  }

  void dumpProbeReceiverReport(const scene::BakeScene &source, const Grid &grid, std::stop_token cancelled) noexcept {
    const char *request = std::getenv("REMIX_DEBUG_PROBE_RECEIVER_RAYS");
    if (!request || !*request) return;
    try {
      const auto path = std::filesystem::u8path(request), outputPath = std::filesystem::u8path(std::string(request) + ".results.json");
      if (std::filesystem::file_size(path) > 4096 || std::filesystem::exists(outputPath))
        throw std::invalid_argument("Probe receiver input exceeds 4 KiB or output already exists");
      std::ifstream input(path);
      input.imbue(std::locale::classic());
      uint32_t count = 0;
      if (!(input >> count) || !count || count > 16) throw std::invalid_argument("Probe receiver count is 1..16");
      std::vector<ReceiverRay> rays(count);
      for (auto &ray : rays)
        for (auto *values : {&ray.origin, &ray.direction})
          for (auto &value : *values) if (!(input >> value)) throw std::invalid_argument("Incomplete probe receiver ray");
      input >> std::ws;
      if (!input.eof()) throw std::invalid_argument("Trailing probe receiver input");
      const auto hits = pickProbeReceivers(source, rays, cancelled);
      const auto origins = probeOrigins(grid);
      std::ostringstream out;
      out.imbue(std::locale::classic());
      out.precision(9);
      out << "{\"version\":1,\"sceneHash\":\"" << source.contentHash << "\",\"sceneScale\":" << source.sceneScale
          << ",\"normalBiasWorldUnits\":" << grid.header.settings[0]
          << ",\"minimumVarianceWorldUnitsSquared\":" << grid.header.settings[1]
          << ",\"directionalDepth\":" << (grid.depth.empty() ? "false" : "true")
          << ",\"rootOriginSpacing\":";
      vector(out, grid.header.originSpacing);
      out << ",\"scope\":\"Rigid opaque triangles and vertex normals; no alpha/normal-map sampling; base grid only, separate styled SH excluded\",\"rays\":[";
      for (size_t i = 0; i < rays.size(); ++i) {
        if (i) out << ',';
        out << "{\"origin\":"; vector(out, rays[i].origin);
        out << ",\"direction\":"; vector(out, rays[i].direction); out << '}';
      }
      out << "],\"hits\":[";
      for (size_t i = 0; i < hits.size(); ++i) {
        check(cancelled);
        if (i) out << ',';
        if (!hits[i]) { out << "null"; continue; }
        const auto &hit = *hits[i];
        const auto &draw = source.draws[hit.draw];
        out << "{\"draw\":" << hit.draw << ",\"firstIndex\":" << hit.firstIndex << ",\"materialSourceHash\":\""
            << (draw.material ? draw.material->sourceHash : 0) << "\",\"distance\":" << hit.distance << ",\"position\":";
        vector(out, hit.position); out << ",\"normal\":"; vector(out, hit.normal);
        out << ",\"geometricNormal\":"; vector(out, hit.geometricNormal);
        const auto value = sample(grid, hit.position, hit.normal);
        out << ",\"irradiance\":"; if (value) vector(out, *value); else out << "null";
        const auto stencil = probeStencil(grid, hit.position, hit.normal);
        float total = 0;
        for (uint32_t c = 0; c < stencil.count; ++c) total += stencil.weights[c];
        out << ",\"stencilCount\":" << stencil.count << ",\"weightSum\":" << total << ",\"probes\":[";
        const auto sh = irradianceBasis(hit.normal);
        bool first = true;
        for (uint32_t c = 0; c < stencil.count; ++c) {
          const float weight = stencil.weights[c];
          if (weight <= 0) continue;
          if (!first) out << ',';
          first = false;
          const uint32_t index = stencil.indices[c];
          const auto &probe = grid.probes.at(index);
          auto position = origins.at(index).position;
          if (uint32_t(grid.header.settings[2]) & 1u)
            for (uint32_t axis = 0; axis < 3; ++axis) position[axis] += probe.irradiance[axis][3];
          Vec3 rawIrradiance{};
          for (uint32_t j = 0; j < 9; ++j)
            for (uint32_t axis = 0; axis < 3; ++axis) rawIrradiance[axis] += probe.irradiance[j][axis] * sh[j];
          out << "{\"index\":" << index << ",\"stencilSlot\":" << c << ",\"weight\":" << weight
              << ",\"normalizedWeight\":" << (total > 0 ? weight / total : 0) << ",\"position\":";
          vector(out, position); out << ",\"rawIrradiance\":"; vector(out, rawIrradiance);
          out << ",\"spacing\":" << origins[index].spacing << ",\"sh\":[";
          for (uint32_t j = 0; j < 9; ++j) {
            if (j) out << ',';
            vector(out, Vec3{probe.irradiance[j][0], probe.irradiance[j][1], probe.irradiance[j][2]});
          }
          out << "],\"moments\":[";
          for (uint32_t j = 0; j < 6; ++j) { if (j) out << ','; vector(out, probe.moments[j]); }
          out << "]}";
        }
        out << "]}";
      }
      out << "]}\n";
      check(cancelled);
      if (std::filesystem::exists(outputPath)) throw std::runtime_error("Probe receiver output already exists");
      std::ofstream output(outputPath);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output << out.str();
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Probe receiver report unavailable: %s\n", error.what());
    }
  }
}
