#include "forward/bake/probe_anchor_report.h"
#include "forward/bake/probe_connection_audit.h"
#include "forward/material.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>

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
    // Diagnostic draw anchor, not an instance anchor: submeshes may be separate draws.
    std::optional<Vec3> drawCenter(const scene::Draw &draw, std::stop_token cancelled) {
      if (!draw.surface) return {};
      const auto &surface = *draw.surface;
      const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
      if (!count || count > 98304 || count % 3 || draw.firstIndex > surface.indices.size() ||
          count > surface.indices.size() - draw.firstIndex) return {};
      Vec3 low, high;
      low.fill(std::numeric_limits<float>::infinity());
      high.fill(-std::numeric_limits<float>::infinity());
      for (size_t i = 0; i < count; ++i) {
        if (i % 1024 == 0) check(cancelled);
        const auto index = int64_t(surface.indices[draw.firstIndex + i]) + draw.vertexOffset;
        const auto &v = surface.vertices.at(size_t(index));
        const auto point = transformPoint(draw.model, {v.position[0], v.position[1], v.position[2]});
        for (uint32_t c = 0; c < 3; ++c) {
          if (!std::isfinite(point[c])) return {};
          low[c] = std::min(low[c], point[c]); high[c] = std::max(high[c], point[c]);
        }
      }
      Vec3 center;
      for (uint32_t c = 0; c < 3; ++c) center[c] = float((double(low[c]) + high[c]) * .5);
      return center;
    }
    void anchorTargetConnections(std::ostream &out, const scene::BakeScene &source, const Grid &grid,
        Vec3 position, uint32_t draw, std::span<const Grid::Origin> origins,
        std::span<const uint32_t> nodes, std::stop_token cancelled) {
      if (nodes.size() > 256) throw std::invalid_argument("Anchor target node budget exceeded");
      out << ",\"targetProbes\":[";
      for (size_t first = 0; first < nodes.size(); first += 16) {
        ProbeStencil stencil{};
        stencil.count = uint32_t(std::min<size_t>(16, nodes.size() - first));
        stencil.receiverPositions.fill(position);
        for (uint32_t c = 0; c < stencil.count; ++c) {
          stencil.indices[c] = nodes[first + c];
          stencil.weights[c] = 1;
        }
        const auto audit = probeConnections(source, grid, stencil, origins, cancelled, draw);
        for (uint32_t c = 0; c < stencil.count; ++c) {
          if (first + c) out << ',';
          out << "{\"index\":" << stencil.indices[c] << ",\"tested\":"
              << (audit.tested[c] ? "true" : "false") << ",\"blocked\":"
              << (audit.blockers[c] ? "true" : "false") << '}';
        }
      }
      out << ']';
    }
    void anchorReceiverConnections(std::ostream &out, const scene::BakeScene &source, Vec3 position,
        uint32_t draw, std::span<const std::optional<ReceiverHit>> hits, std::stop_token cancelled) {
      using State = scene::VisibilityPartition::SegmentState;
      std::vector<ReceiverRay> rays;
      std::vector<uint32_t> slots;
      std::array<Vec3, 16> endpoints{};
      std::array<float, 16> lengths{};
      const float epsilon = .0001f / source.sceneScale;
      for (uint32_t i = 0; i < hits.size(); ++i) {
        if (!hits[i] || hits[i]->draw != draw) continue;
        ReceiverRay ray{position};
        for (uint32_t c = 0; c < 3; ++c) {
          endpoints[i][c] = hits[i]->position[c] + hits[i]->geometricNormal[c] * (.002f / source.sceneScale);
          ray.direction[c] = endpoints[i][c] - position[c];
        }
        const float distance = std::sqrt(dot(ray.direction, ray.direction));
        if (!std::isfinite(distance) || distance <= 2 * epsilon) continue;
        for (uint32_t c = 0; c < 3; ++c) {
          ray.direction[c] /= distance;
          ray.origin[c] += ray.direction[c] * epsilon;
        }
        rays.push_back(ray); slots.push_back(i); lengths[i] = distance - 2 * epsilon;
      }
      std::vector<std::optional<ReceiverHit>> blockers;
      if (!rays.empty()) blockers = pickProbeReceivers(source, rays, cancelled, draw);
      out << ",\"receivers\":[";
      for (size_t c = 0; c < slots.size(); ++c) {
        const auto slot = slots[c];
        const auto interval = source.partition ? source.partition->firstSolid(position.data(), endpoints[slot].data()) :
            scene::VisibilityPartition::SegmentResult{};
        if (c) out << ',';
        out << "{\"ray\":" << slot << ",\"endpoint\":"; vector(out, endpoints[slot]);
        out << ",\"partition\":\"" << (interval.state == State::Clear ? "clear" : interval.state == State::Solid ? "solid" : "unknown")
            << "\",\"opaqueBlocked\":" << (blockers[c] && blockers[c]->distance < lengths[slot] ? "true" : "false") << '}';
      }
      out << ']';
    }
    void surfaceAnchors(std::ostream &out, const scene::BakeScene &source, uint32_t drawIndex,
        const Grid &grid, std::span<const Grid::Origin> origins, std::span<const uint32_t> nodes,
        std::span<const std::optional<ReceiverHit>> hits, std::stop_token cancelled) {
      struct Candidate { Vec3 position{}; double area = 0; size_t first = 0; int32_t cluster = -2; };
      std::array<Candidate, 6> candidates{};
      const auto &draw = source.draws[drawIndex];
      const auto &surface = *draw.surface;
      const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
      out << ",\"surfaceAnchors\":[";
      // Same complete-geometry budget as the midpoint; never rank a truncated prefix.
      if (!count || count > 98304 || count % 3 || draw.firstIndex > surface.indices.size() ||
          count > surface.indices.size() - draw.firstIndex || !(source.sceneScale > 0) ||
          !std::isfinite(source.sceneScale)) { out << ']'; return; }
      const auto normals = normalMatrix(draw.model);
      for (size_t first = draw.firstIndex; first < draw.firstIndex + count; first += 3) {
        if (first % 1024 < 3) check(cancelled);
        std::array<Vec3, 3> points;
        Vec3 shading{};
        for (uint32_t c = 0; c < 3; ++c) {
          const auto &v = surface.vertices.at(size_t(int64_t(surface.indices[first + c]) + draw.vertexOffset));
          points[c] = transformPoint(draw.model, {v.position[0], v.position[1], v.position[2]});
          const auto n = transformVector(normals, {v.normal[0], v.normal[1], v.normal[2]});
          for (uint32_t axis = 0; axis < 3; ++axis) shading[axis] += n[axis];
        }
        auto normal = crossDouble(difference(points[1], points[0]), difference(points[2], points[0]));
        const double area = std::sqrt(dotDouble(normal, normal));
        if (!(area > 0) || !std::isfinite(area)) continue;
        const double alignment = normal[0]*shading[0] + normal[1]*shading[1] + normal[2]*shading[2];
        if (!std::isfinite(alignment) || std::abs(alignment) < area * 1e-8) continue;
        for (auto &value : normal) value *= (alignment < 0 ? -1 : 1) / area;
        uint32_t axis = 0;
        for (uint32_t c = 1; c < 3; ++c)
          if (std::abs(normal[c]) > std::abs(normal[axis])) axis = c;
        auto &candidate = candidates[axis * 2 + (normal[axis] < 0)];
        if (area <= candidate.area) continue;
        Vec3 position;
        bool finite = true;
        for (uint32_t c = 0; c < 3; ++c) {
          position[c] = float((double(points[0][c]) + points[1][c] + points[2][c]) / 3 +
              normal[c] * (.002 / source.sceneScale));
          finite &= std::isfinite(position[c]);
        }
        if (!finite) continue;
        const auto cluster = source.partition && source.partition->closed ? source.partition->cluster(position.data()) : -2;
        // Only independently classified playable surfaces compete for an anchor.
        if (cluster < 0) continue;
        candidate = {position, area, first, cluster};
      }
      bool emitted = false;
      for (uint32_t direction = 0; direction < candidates.size(); ++direction) {
        const auto &candidate = candidates[direction];
        if (!candidate.area) continue;
        if (emitted) out << ',';
        emitted = true;
        out << "{\"direction\":" << direction << ",\"firstIndex\":" << candidate.first
            << ",\"position\":"; vector(out, candidate.position);
        out << ",\"partitionCluster\":" << candidate.cluster << ",\"probes\":[";
        const auto stencil = probeStencil(grid, candidate.position, Vec3{});
        const auto audit = probeConnections(source, grid, stencil, origins, cancelled, drawIndex);
        for (uint32_t c = 0; c < stencil.count; ++c) {
          if (c) out << ',';
          out << "{\"index\":" << stencil.indices[c] << ",\"tested\":"
              << (audit.tested[c] ? "true" : "false") << ",\"blocked\":"
              << (audit.blockers[c] ? "true" : "false") << '}';
        }
        out << ']';
        anchorTargetConnections(out, source, grid, candidate.position, drawIndex, origins, nodes, cancelled);
        anchorReceiverConnections(out, source, candidate.position, drawIndex, hits, cancelled);
        out << '}';
      }
      out << ']';
    }
  }
  void dumpProbeAnchorReport(std::ostream &out, const scene::BakeScene &source, const Grid &grid,
      std::span<const Grid::Origin> origins, std::span<const std::optional<ReceiverHit>> hits,
      std::stop_token cancelled) {
    std::vector<uint32_t> draws;
    out << ",\"drawAnchors\":[";
    for (const auto &hit : hits) {
      if (!hit || std::find(draws.begin(), draws.end(), hit->draw) != draws.end()) continue;
      if (!draws.empty()) out << ',';
      draws.push_back(hit->draw);
      const auto center = drawCenter(source.draws[hit->draw], cancelled);
      out << "{\"draw\":" << hit->draw << ",\"center\":";
      if (center) vector(out, *center); else out << "null";
      const auto cluster = center && source.partition && source.partition->closed ?
          source.partition->cluster(center->data()) : -2;
      out << ",\"partitionCluster\":" << cluster
          << ",\"scope\":\"Bounded draw AABB midpoint; no normal bias; self draw excluded; anchor stencil only\",\"probes\":[";
      if (center && cluster != -1) {
        const auto stencil = probeStencil(grid, *center, Vec3{});
        const auto audit = probeConnections(source, grid, stencil, origins, cancelled, hit->draw);
        for (uint32_t c = 0; c < stencil.count; ++c) {
          if (c) out << ',';
          out << "{\"index\":" << stencil.indices[c] << ",\"tested\":"
              << (audit.tested[c] ? "true" : "false") << ",\"blocked\":"
              << (audit.blockers[c] ? "true" : "false") << '}';
        }
      }
      out << ']';
      std::vector<uint32_t> nodes;
      for (const auto &target : hits) {
        if (!target || target->draw != hit->draw) continue;
        const auto stencil = probeStencil(grid, target->position, target->normal);
        for (uint32_t c = 0; c < stencil.count; ++c)
          if (stencil.weights[c] > 0 && std::find(nodes.begin(), nodes.end(), stencil.indices[c]) == nodes.end())
            nodes.push_back(stencil.indices[c]);
      }
      surfaceAnchors(out, source, hit->draw, grid, origins, nodes, hits, cancelled);
      out << '}';
    }
    out << ']';
  }
}
