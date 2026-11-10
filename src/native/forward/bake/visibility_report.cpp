#include "forward/bake/visibility_report.h"
#include "forward/mixed/data.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>

namespace remix::forward::bake {
  namespace {
    void writeVector(std::ostream &out, const Vec3 &value) {
      out << '[' << value[0] << ',' << value[1] << ',' << value[2] << ']';
    }
    void writeBounds(std::ostream &out, const Bounds &value) {
      if (!value.valid) { out << "null"; return; }
      out << '[';
      writeVector(out, value.minimum);
      out << ',';
      writeVector(out, value.maximum);
      out << ']';
    }
  }
  void dumpVisibilityReport(const mixed::Plan &plan, const lightmap::Layout &layout,
                            float receiverBias, std::stop_token cancelled) noexcept {
    const char *name = std::getenv("REMIX_DEBUG_BAKE_VISIBILITY_REPORT");
    if (!name || !*name || !plan.lights.source || !plan.lights.source->partition) return;
    try {
      const auto &source = *plan.lights.source;
      const auto &partition = *source.partition;
      const size_t count = source.charts.size();
      std::vector<Bounds> unshifted(count), biased(count);
      std::vector<uint32_t> points(count), receivers(count);
      for (size_t i = 0; i < layout.points.size(); ++i) {
        if ((i & 1023u) == 0 && cancelled.stop_requested()) return;
        const auto chart = layout.pointCharts.at(i);
        const auto &point = layout.points[i];
        Vec3 p{point.position[0], point.position[1], point.position[2]};
        unshifted.at(chart).include(p);
        const Vec3 normal{point.normal[0], point.normal[1], point.normal[2]};
        const float length2 = dot(normal, normal);
        if (length2 > 1e-10f) {
          const float scale = receiverBias / std::sqrt(length2);
          for (uint32_t axis = 0; axis < 3; ++axis) p[axis] += normal[axis] * scale;
        }
        biased.at(chart).include(p);
        ++points.at(chart);
        receivers.at(chart) += layout.receiverSamples.empty() || layout.receiverSamples.at(i);
      }
      const std::filesystem::path path(name);
      if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
      std::ofstream out(path);
      out.exceptions(std::ios::failbit | std::ios::badbit);
      out.imbue(std::locale::classic());
      out << std::setprecision(std::numeric_limits<float>::max_digits10);
      out << "{\"version\":2,\"source\":\"" << source.contentHash << "\",\"receiverBias\":" << receiverBias
          << ",\"partition\":{\"closed\":" << (partition.closed ? "true" : "false")
          << ",\"clusters\":" << partition.clusterCount << ",\"rowBytes\":" << partition.rowBytes << ",\"nodes\":[";
      for (size_t i = 0; i < partition.nodes.size(); ++i) {
        if ((i & 1023u) == 0 && cancelled.stop_requested()) return;
        if (i) out << ',';
        const auto &node = partition.nodes[i];
        out << '[' << node.plane[0] << ',' << node.plane[1] << ',' << node.plane[2] << ',' << node.plane[3]
            << ',' << node.children[0] << ',' << node.children[1] << ']';
      }
      out << "],\"leaves\":[";
      for (size_t i = 0; i < partition.leafClusters.size(); ++i) {
        if ((i & 1023u) == 0 && cancelled.stop_requested()) return;
        if (i) out << ',';
        out << partition.leafClusters[i];
      }
      out << "],\"visibility\":\"";
      constexpr char hex[] = "0123456789abcdef";
      for (size_t i = 0; i < partition.visibility.size(); ++i) {
        if ((i & 65535u) == 0 && cancelled.stop_requested()) return;
        const auto byte = partition.visibility[i];
        out << hex[byte >> 4] << hex[byte & 15];
      }
      out << "\"},\"lights\":[";
      for (size_t i = 0; i < source.lights.size(); ++i) {
        if ((i & 1023u) == 0 && cancelled.stop_requested()) return;
        if (i) out << ',';
        const auto &light = source.lights[i];
        Bounds emitter;
        if (light.triangle) {
          for (const auto &corner : light.corners) emitter.include({corner[0], corner[1], corner[2]});
        } else {
          emitter.include({light.position[0] - light.radius, light.position[1] - light.radius,
                           light.position[2] - light.radius});
          emitter.include({light.position[0] + light.radius, light.position[1] + light.radius,
                           light.position[2] + light.radius});
        }
        out << "{\"style\":" << light.style << ",\"triangle\":" << (light.triangle ? "true" : "false")
            << ",\"coneCos\":" << light.coneCos << ",\"focus\":" << light.focus << ",\"bounds\":";
        writeBounds(out, emitter);
        out << '}';
      }
      std::vector<std::set<uint32_t>> masks(count);
      for (const auto &entry : plan.masks.entries()) masks.at(entry.chart).insert(entry.light);
      out << "],\"charts\":[";
      for (size_t i = 0; i < count; ++i) {
        if (cancelled.stop_requested()) return;
        if (i) out << ',';
        auto broad = unshifted[i];
        const auto &chart = source.charts[i];
        const auto &draw = source.draws.at(chart.draw);
        for (size_t corner = 0; corner < chart.texcoords.size(); ++corner) {
          const auto index = int64_t(draw.surface->indices.at(chart.firstIndex + corner)) + draw.vertexOffset;
          const auto &p = draw.surface->vertices.at(size_t(index)).position;
          broad.include(transformPoint(draw.model, {p[0], p[1], p[2]}));
        }
        if (broad.valid)
          for (uint32_t axis = 0; axis < 3; ++axis) {
            broad.minimum[axis] -= receiverBias;
            broad.maximum[axis] += receiverBias;
          }
        out << "{\"points\":" << points[i] << ",\"receivers\":" << receivers[i] << ",\"bounds\":";
        writeBounds(out, broad);
        out << ",\"biasedPoints\":";
        writeBounds(out, biased[i]);
        out << ",\"candidates\":[";
        auto candidates = masks[i];
        candidates.insert(plan.remaining.at(i).begin(), plan.remaining.at(i).end());
        for (const auto light : plan.charts.at(i)) if (light != mixed::none) candidates.insert(light);
        for (const auto light : plan.directCharts.at(i)) if (light != mixed::none) candidates.insert(light);
        for (uint32_t light = 0; light < source.lights.size(); ++light)
          if (plan.occlusion.state(uint32_t(i), light) != mixed::Visibility::Unknown) candidates.insert(light);
        bool first = true;
        for (const auto light : candidates) {
          if (!first) out << ',';
          first = false;
          out << '[' << light << ',' << uint32_t(plan.occlusion.state(uint32_t(i), light)) << ']';
        }
        out << "]}";
      }
      out << "]}\n";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Bake visibility report unavailable: %s\n", error.what());
    }
  }
}
