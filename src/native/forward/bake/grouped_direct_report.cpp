#include "forward/bake/receiver_report.h"
#include "forward/bake/styled_direct.h"
#include "forward/bake/spherical_harmonics.h"
#include "forward/mixed/directional_fields.h"
#include "forward/lighting.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace remix::forward::bake {
  namespace {
    float fieldValue(std::span<const uint32_t> words, const lightmap::Layout &layout,
                       const ReceiverComparison &receiver, uint32_t light) {
      const auto &r = layout.charts.at(receiver.chart);
      const float x = std::clamp(receiver.coordinate[0] * layout.pageSize - .5f - r.x, 0.f, float(r.width - 1));
      const float y = std::clamp(receiver.coordinate[1] * layout.pageSize - .5f - r.y, 0.f, float(r.height - 1));
      const uint32_t lowX = uint32_t(x), lowY = uint32_t(y);
      const auto sh = irradianceBasis(receiver.normal);
      float values[4]{};
      for (uint32_t tap = 0; tap < 4; ++tap) {
        const auto field = mixed::DirectionalFields::sample(words, receiver.chart, light,
            std::min(lowX + (tap & 1u), r.width - 1), std::min(lowY + (tap >> 1u), r.height - 1));
        if (!field) return 0;
        for (uint32_t c = 0; c < 9; ++c) values[tap] += half::decode((*field)[c]) * sh[c];
      }
      return std::max(std::lerp(std::lerp(values[0], values[1], x - lowX),
                                std::lerp(values[2], values[3], x - lowX), y - lowY), 0.f);
    }
    float above(const scene::SphereLight &light, Vec3 p, Vec3 n) {
      if (!light.triangle) {
        Vec3 delta;
        for (uint32_t c = 0; c < 3; ++c) delta[c] = light.position[c] - p[c];
        return dot(delta, n) + light.radius;
      }
      float support = -INFINITY;
      for (const auto &corner : light.corners) {
        Vec3 delta;
        for (uint32_t c = 0; c < 3; ++c) delta[c] = corner[c] - p[c];
        support = std::max(support, dot(delta, n));
      }
      return support;
    }
    void vector(std::ostream &out, Vec3 v) { out << '[' << v[0] << ',' << v[1] << ',' << v[2] << ']'; }
  }
  void logGroupedDirectDeclines(const scene::BakeScene &source, const ProbeVisibility &probes,
                                const StyledDirectGroups &groups) noexcept {
    if (groups.declined.empty()) return;
    try {
      // Inspect only the reported owners, in one pass regardless of style count.
      std::map<uint32_t, std::array<size_t, 2>> probeCoverage;
      for (const auto &declined : groups.declined)
        if (declined.reason == StyledDirectDecline::ProbeCoverage && declined.light != UINT32_MAX)
          probeCoverage.try_emplace(declined.light);
      if (!probeCoverage.empty())
        for (const auto &entry : probes.entries())
          if (const auto found = probeCoverage.find(entry.light); found != probeCoverage.end())
            ++found->second[entry.visibility < 0 ? 1 : 0];
      for (const auto &declined : groups.declined) {
        const char *reason = "";
        switch (declined.reason) {
        case StyledDirectDecline::SurfaceCoverage: reason = "incomplete surface coverage"; break;
        case StyledDirectDecline::ProbeCoverage: reason = "incomplete probe coverage"; break;
        case StyledDirectDecline::Storage: reason = "storage budget"; break;
        }
        std::fprintf(stderr, "Forward grouped direct style %u declined: %s; retaining per-light fallback\n",
                     declined.style, reason);
        if (declined.light != UINT32_MAX) {
          const auto &light = source.lights.at(declined.light);
          std::fprintf(stderr, "  captured light %u at (%.3f, %.3f, %.3f)\n",
                       declined.light, light.position[0], light.position[1], light.position[2]);
          if (const auto found = probeCoverage.find(declined.light); found != probeCoverage.end())
            std::fprintf(stderr, "  known probe pairs %zu, unknown %zu\n", found->second[0], found->second[1]);
        }
      }
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Grouped direct decline details unavailable: %s\n", error.what());
    }
  }
  void dumpGroupedDirectReport(const scene::BakeScene &source, const lightmap::Layout &layout,
      std::span<const uint16_t> pixels, const Grid &grid, std::span<const uint32_t> directionals,
      const StyledDirectGroups &groups, std::stop_token cancelled) noexcept {
    const char *path = std::getenv("REMIX_DEBUG_BAKE_RECEIVERS");
    if (!path || !*path || groups.styles.empty()) return;
    try {
      const auto outputPath = std::filesystem::path(std::string(path) + ".grouped-direct.json");
      if (std::filesystem::exists(outputPath)) throw std::runtime_error("Grouped receiver report already exists");
      const auto receivers = compareReceivers(source, layout, pixels, grid, 8192, cancelled);
      std::vector<StyleScale> scales;
      for (uint32_t style : groups.styles) scales.push_back({style, 1, StyleStatus::Ready});
      std::ofstream out(outputPath);
      out.exceptions(std::ios::failbit | std::ios::badbit);
      out.precision(9);
      out << "{\"version\":1,\"scope\":\"Authored full-on grouped direct versus individual captured SH; triangle-centre vertex normals; no materials, normal maps, range fade or dynamic shadows\",\"receivers\":[";
      bool first = true;
      for (const auto &original : receivers) for (bool flipped : {false, true}) {
        auto receiver = original;
        if (flipped) for (auto &c : receiver.normal) c = -c;
        if (cancelled.stop_requested()) throw std::runtime_error("Grouped receiver report superseded");
        Vec3 unclipped{}, horizon{};
        uint32_t behind = 0;
        for (uint32_t i = 0; i < source.lights.size(); ++i) {
          const auto &light = source.lights[i];
          if (!std::binary_search(groups.styles.begin(), groups.styles.end(), light.style)) continue;
          const float value = fieldValue(directionals, layout, receiver, i);
          const bool visibleHemisphere = above(light, receiver.position, receiver.normal) > 0;
          if (value > 0 && !visibleHemisphere) ++behind;
          for (uint32_t c = 0; c < 3; ++c) {
            unclipped[c] += value * light.radiance[c];
            if (visibleHemisphere) horizon[c] += value * light.radiance[c];
          }
        }
        const auto grouped = sparse::surfaceIrradiance(groups.words, scales, receiver.coordinate, receiver.normal);
        if (std::max({grouped[0], grouped[1], grouped[2], unclipped[0], unclipped[1], unclipped[2]}) < 1e-6f) continue;
        out << (first ? "" : ",") << "\n{\"chart\":" << receiver.chart << ",\"flippedNormal\":" << (flipped ? "true" : "false") << ",\"position\":";
        vector(out, receiver.position); out << ",\"normal\":"; vector(out, receiver.normal);
        out << ",\"grouped\":"; vector(out, grouped); out << ",\"individual\":"; vector(out, unclipped);
        out << ",\"horizonClipped\":"; vector(out, horizon); out << ",\"behindContributors\":" << behind << '}';
        first = false;
      }
      out << "\n]}";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Grouped receiver report unavailable: %s\n", error.what());
    }
  }
}
