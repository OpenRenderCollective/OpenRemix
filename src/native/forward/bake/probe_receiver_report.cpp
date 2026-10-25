#include "forward/bake/probe_receiver_report.h"
#include "forward/bake/probe_receiver_geometry.h"
#include "forward/bake/model_receivers.h"
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
    void receiverGeometry(std::ostream &out, const scene::BakeScene &source, uint32_t drawIndex,
                          std::stop_token cancelled) {
      const auto &draw = source.draws[drawIndex];
      const auto &surface = *draw.surface;
      const auto normals = normalMatrix(draw.model);
      const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
      const size_t triangles = std::min<size_t>(count / 3, 256);
      out << ",\"geometry\":{\"totalTriangles\":" << count / 3
          << ",\"truncated\":" << (triangles < count / 3 ? "true" : "false") << ",\"charts\":[";
      uint32_t emitted = 0;
      for (uint32_t i = 0; i < source.charts.size(); ++i) {
        check(cancelled);
        const auto &chart = source.charts[i];
        if (chart.draw != drawIndex) continue;
        if (emitted == 256) break;
        if (emitted++) out << ',';
        out << "{\"chart\":" << i << ",\"firstIndex\":" << chart.firstIndex
            << ",\"indexCount\":" << chart.texcoords.size() << '}';
      }
      const auto chartCount = std::count_if(source.charts.begin(), source.charts.end(),
          [&](const auto &chart) { return chart.draw == drawIndex; });
      out << "],\"totalCharts\":" << chartCount << ",\"chartsTruncated\":"
          << (chartCount > emitted ? "true" : "false") << ",\"triangles\":[";
      for (size_t triangle = 0; triangle < triangles; ++triangle) {
        check(cancelled);
        const auto first = draw.firstIndex + triangle * 3;
        if (triangle) out << ',';
        out << "{\"firstIndex\":" << first << ",\"corners\":[";
        for (uint32_t c = 0; c < 3; ++c) {
          const auto index = int64_t(surface.indices[first + c]) + draw.vertexOffset;
          const auto &v = surface.vertices.at(size_t(index));
          const auto p = transformPoint(draw.model, {v.position[0], v.position[1], v.position[2]});
          const auto n = transformVector(normals, {v.normal[0], v.normal[1], v.normal[2]});
          for (uint32_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(p[axis]) || !std::isfinite(n[axis]))
              throw std::invalid_argument("Nonfinite receiver geometry");
          if (c) out << ',';
          out << "{\"vertex\":" << index << ",\"position\":"; vector(out, p);
          out << ",\"normal\":"; vector(out, n); out << '}';
        }
        out << "]}";
      }
      out << "]}";
    }
    struct ConnectionAudit {
      std::array<Vec3, 16> positions{};
      std::array<bool, 16> tested{};
      std::array<std::optional<ReceiverHit>, 16> blockers{};
    };
    ConnectionAudit probeConnections(const scene::BakeScene &source, const Grid &grid,
        const ProbeStencil &stencil, std::span<const Grid::Origin> origins, std::stop_token cancelled) {
      if (!(source.sceneScale > 0) || !std::isfinite(source.sceneScale))
        throw std::invalid_argument("Invalid probe connection scene scale");
      ConnectionAudit result;
      std::array<float, 16> lengths{};
      std::vector<ReceiverRay> rays;
      std::vector<uint32_t> slots;
      // Geometry-only evidence, not alpha/transmission visibility. Keep the
      // exact biased endpoint, excluding both endpoints by 0.1 mm.
      const float tolerance = .0001f / source.sceneScale;
      for (uint32_t c = 0; c < stencil.count; ++c) {
        if (stencil.weights[c] <= 0) continue;
        const auto index = stencil.indices[c];
        if (index >= origins.size()) throw std::invalid_argument("Invalid probe connection origin");
        auto &position = result.positions[c];
        position = origins[index].position;
        if (uint32_t(grid.header.settings[2]) & 1u)
          for (uint32_t axis = 0; axis < 3; ++axis) position[axis] += grid.probes.at(index).irradiance[axis][3];
        Vec3 direction;
        for (uint32_t axis = 0; axis < 3; ++axis)
          direction[axis] = stencil.receiverPositions[c / 8][axis] - position[axis];
        const float distance = std::sqrt(dot(direction, direction));
        if (!std::isfinite(distance) || distance <= 2 * tolerance) continue;
        ReceiverRay ray;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          ray.direction[axis] = direction[axis] / distance;
          ray.origin[axis] = position[axis] + ray.direction[axis] * tolerance;
        }
        rays.push_back(ray); slots.push_back(c);
        lengths[c] = distance - 2 * tolerance;
        result.tested[c] = true;
      }
      if (!rays.empty()) {
        const auto hits = pickProbeReceivers(source, rays, cancelled);
        for (size_t c = 0; c < slots.size(); ++c)
          if (hits[c] && hits[c]->distance < lengths[slots[c]]) result.blockers[slots[c]] = hits[c];
      }
      return result;
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

  std::optional<float> partitionContainedBias(const scene::BakeScene &source,
      Vec3 position, Vec3 normal, float bias) {
    if (!source.partition || !source.partition->closed || !(source.sceneScale > 0) ||
        !std::isfinite(source.sceneScale) || !std::isfinite(bias) || bias < 0) return {};
    const float epsilon = .0001f / source.sceneScale, initial = std::min(epsilon, bias);
    Vec3 start, end;
    for (uint32_t c = 0; c < 3; ++c) {
      start[c] = position[c] + normal[c] * initial;
      end[c] = position[c] + normal[c] * bias;
    }
    const auto interval = source.partition->firstSolid(start.data(), end.data());
    using State = scene::VisibilityPartition::SegmentState;
    if (interval.state == State::Clear) return bias;
    if (interval.state != State::Solid || interval.fraction <= 0) return {};
    return std::max(0.f, float(initial + (bias - initial) * interval.fraction) - epsilon);
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
      if (!opaqueProbeReceiver(draw)) continue;
      const auto &surface = *draw.surface;
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

  void dumpModelReceiverReport(const scene::BakeScene &source, const Grid &grid, std::stop_token cancelled) noexcept {
    const char *request = std::getenv("REMIX_DEBUG_BAKE_MODEL_RECEIVERS");
    if (!request || !*request) return;
    try {
      const auto path = std::filesystem::u8path(request);
      if (std::filesystem::exists(path)) throw std::runtime_error("Model receiver output already exists");
      const auto sites = modelReceivers(source, 4096, cancelled);
      std::ostringstream out;
      out.imbue(std::locale::classic());
      out.precision(9);
      out << "{\"version\":1,\"sceneHash\":\"" << source.contentHash << "\",\"sceneScale\":" << source.sceneScale
          << ",\"scope\":\"Uncharted rigid opaque triangle-centre geometry and current base probe estimates; no alpha texels, normal maps or independent lighting reference\",\"sites\":[";
      for (size_t i = 0; i < sites.size(); ++i) {
        check(cancelled);
        const auto &site = sites[i];
        const Vec3 position{site.point.position[0], site.point.position[1], site.point.position[2]};
        const Vec3 normal{site.point.normal[0], site.point.normal[1], site.point.normal[2]};
        out << (i ? "," : "") << "{\"draw\":" << site.draw << ",\"firstIndex\":" << site.firstIndex << ",\"position\":";
        vector(out, position); out << ",\"normal\":"; vector(out, normal);
        out << ",\"cluster\":";
        if (source.partition && source.partition->closed) out << source.partition->cluster(position.data());
        else out << "null";
        out << ",\"probe\":";
        if (const auto value = sample(grid, position, normal)) vector(out, *value);
        else out << "null";
        out << '}';
      }
      out << "]}\n";
      check(cancelled);
      const auto report = out.str();
      if (std::filesystem::exists(path)) throw std::runtime_error("Model receiver output already exists");
      std::ofstream output(path);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output << report;
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Model receiver report unavailable: %s\n", error.what());
    }
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
      std::optional<Grid> containmentGrid;
      if (source.partition && source.partition->closed && grid.probes.size() <= 65536) containmentGrid = grid;
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
        receiverGeometry(out, source, hit.draw, cancelled);
        const auto value = sample(grid, hit.position, hit.normal);
        out << ",\"irradiance\":"; if (value) vector(out, *value); else out << "null";
        std::optional<float> containedBias;
        if (containmentGrid) containedBias = partitionContainedBias(source, hit.position, hit.normal, grid.header.settings[0]);
        out << ",\"containedBiasWorldUnits\":";
        if (containedBias) {
          out << *containedBias;
          containmentGrid->header.settings[0] = *containedBias;
          const auto containedValue = sample(*containmentGrid, hit.position, hit.normal);
          out << ",\"containedIrradiance\":";
          if (containedValue) vector(out, *containedValue); else out << "null";
          const auto containedStencil = probeStencil(*containmentGrid, hit.position, hit.normal);
          const auto containedAudit = probeConnections(source, *containmentGrid, containedStencil, origins, cancelled);
          float sum = 0, blocked = 0;
          for (uint32_t c = 0; c < containedStencil.count; ++c) {
            sum += containedStencil.weights[c];
            if (containedAudit.blockers[c]) blocked += containedStencil.weights[c];
          }
          out << ",\"containedOpaqueBlockedWeight\":" << (sum > 0 ? blocked / sum : 0);

        } else out << "null,\"containedIrradiance\":null";
        const auto stencil = probeStencil(grid, hit.position, hit.normal);
        float total = 0;
        for (uint32_t c = 0; c < stencil.count; ++c) total += stencil.weights[c];
        const auto audit = probeConnections(source, grid, stencil, origins, cancelled);
        float blockedWeight = 0;
        for (uint32_t c = 0; c < stencil.count; ++c)
          if (audit.blockers[c] && total > 0) blockedWeight += stencil.weights[c] / total;
        out << ",\"opaqueBlockedWeight\":" << blockedWeight;
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
          const auto &position = audit.positions[c];
          Vec3 rawIrradiance{};
          for (uint32_t j = 0; j < 9; ++j)
            for (uint32_t axis = 0; axis < 3; ++axis) rawIrradiance[axis] += probe.irradiance[j][axis] * sh[j];
          out << "{\"index\":" << index << ",\"stencilSlot\":" << c << ",\"weight\":" << weight
              << ",\"normalizedWeight\":" << (total > 0 ? weight / total : 0) << ",\"position\":";
          vector(out, position);
          out << ",\"receiverPosition\":"; vector(out, stencil.receiverPositions[c / 8]);
          out << ",\"opaqueConnectionTested\":" << (audit.tested[c] ? "true" : "false")
              << ",\"opaqueConnectionBlocked\":" << (audit.blockers[c] ? "true" : "false");
          out << ",\"opaqueBlocker\":";
          if (const auto &blocker = audit.blockers[c]) {
            out << "{\"draw\":" << blocker->draw << ",\"firstIndex\":" << blocker->firstIndex
                << ",\"position\":"; vector(out, blocker->position); out << '}';
          } else out << "null";
          out << ",\"rawIrradiance\":"; vector(out, rawIrradiance);
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
