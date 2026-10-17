#include "forward/bake/receiver_report.h"
#include "forward/bake/spherical_harmonics.h"
#include "forward/material.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    float half(uint16_t value) {
      const int exponent = (value >> 10) & 31;
      const float mantissa = float(value & 1023);
      if (exponent == 31)
        throw std::invalid_argument("Nonfinite receiver atlas coefficient");
      const float magnitude = exponent ? std::ldexp(1024 + mantissa, exponent - 25)
                                       : std::ldexp(mantissa, -24);
      return value & 0x8000 ? -magnitude : magnitude;
    }
    Vec3 atlasIrradiance(const lightmap::Layout &layout, std::span<const uint16_t> pixels,
                         uint32_t chart, Vec4 coordinate, Vec3 normal) {
      const auto &a = layout.charts.at(chart);
      const auto sh = irradianceBasis(normal);
      const float x = std::clamp(coordinate[0] * layout.pageSize - .5f, float(a.x), float(a.x + a.width - 1));
      const float y = std::clamp(coordinate[1] * layout.pageSize - .5f, float(a.y), float(a.y + a.height - 1));
      const uint32_t x0 = uint32_t(x), y0 = uint32_t(y),
                     x1 = std::min(x0 + 1, a.x + a.width - 1),
                     y1 = std::min(y0 + 1, a.y + a.height - 1);
      Vec3 result{};
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient) {
        const size_t layer = size_t(a.page) * lightmap::atlasLayers + coefficient;
        for (uint32_t channel = 0; channel < 3; ++channel) {
          auto texel = [&](uint32_t px, uint32_t py) {
            return half(pixels[((layer * layout.pageSize + py) * layout.pageSize + px) * 4 + channel]);
          };
          const float row0 = std::lerp(texel(x0, y0), texel(x1, y0), x - x0);
          const float row1 = std::lerp(texel(x0, y1), texel(x1, y1), x - x0);
          result[channel] += sh[coefficient] * std::lerp(row0, row1, y - y0);
        }
      }
      for (float &channel : result) channel = std::max(channel, 0.f);
      return result;
    }
    bool charted(const lightmap::Mesh &mesh, size_t first) {
      const auto a = mesh.vertices[mesh.indices[first]].coordinate[3];
      return a >= .5f && a == mesh.vertices[mesh.indices[first + 1]].coordinate[3] &&
             a == mesh.vertices[mesh.indices[first + 2]].coordinate[3];
    }
    double luminance(Vec3 value) {
      return .2126 * value[0] + .7152 * value[1] + .0722 * value[2];
    }
    void vector(std::ostream &output, Vec3 value) {
      output << '[' << value[0] << ',' << value[1] << ',' << value[2] << ']';
    }
  } // namespace
  std::vector<ReceiverComparison> compareReceivers(const scene::BakeScene &source,
      const lightmap::Layout &layout, std::span<const uint16_t> pixels, const Grid &grid,
      uint32_t maximumSamples, std::stop_token cancelled) {
    const uint64_t expected = uint64_t(layout.pageSize) * layout.pageSize * layout.pages *
                              lightmap::atlasLayers * 4;
    if (!layout.pageSize || pixels.size() != expected || !maximumSamples || maximumSamples > 65536)
      throw std::invalid_argument("Invalid receiver comparison inputs");
    for (const auto &a : layout.charts)
      if (!a.width || !a.height || a.page >= layout.pages || a.width > layout.pageSize ||
          a.height > layout.pageSize || a.x > layout.pageSize - a.width || a.y > layout.pageSize - a.height)
        throw std::invalid_argument("Invalid receiver chart allocation");
    const auto meshes = lightmap::meshes(source, layout, cancelled);
    uint64_t triangles = 0;
    for (const auto &mesh : meshes)
      for (size_t first = 0; first + 2 < mesh.indices.size(); first += 3)
        triangles += charted(mesh, first);
    const auto count = uint32_t(std::min(uint64_t(maximumSamples), triangles));
    std::vector<ReceiverComparison> result;
    result.reserve(count);
    uint64_t ordinal = 0;
    for (const auto &mesh : meshes) {
      const auto &draw = source.draws.at(mesh.draw);
      const auto normalTransform = normalMatrix(draw.model);
      for (size_t first = 0; first + 2 < mesh.indices.size() && result.size() < count; first += 3) {
        if (cancelled.stop_requested())
          throw std::runtime_error("Receiver comparison superseded");
        if (!charted(mesh, first)) continue;
        const uint64_t target = (uint64_t(result.size()) * triangles + triangles / 2) / count;
        if (ordinal++ != target) continue;
        ReceiverComparison receiver;
        receiver.draw = mesh.draw;
        Vec4 coordinate{};
        for (uint32_t corner = 0; corner < 3; ++corner) {
          const auto &vertex = mesh.vertices[mesh.indices[first + corner]];
          const auto &p = vertex.surface.position, &n = vertex.surface.normal;
          const auto position = transformPoint(draw.model, {p[0], p[1], p[2]});
          const auto normal = transformVector(normalTransform, {n[0], n[1], n[2]});
          for (uint32_t c = 0; c < 3; ++c) {
            receiver.position[c] += position[c] / 3;
            receiver.normal[c] += normal[c] / 3;
          }
          for (uint32_t c = 0; c < 4; ++c) coordinate[c] += vertex.coordinate[c] / 3;
        }
        receiver.chart = uint32_t(mesh.vertices[mesh.indices[first]].coordinate[3]) - 1;
        receiver.normal = normalized(receiver.normal);
        if (source.partition && source.partition->closed) {
          Vec3 exterior = receiver.position;
          // Classify just off the smooth surface, avoiding a BSP plane tie.
          // This is provenance only; sampling still uses the original position
          // and the exact published probe-normal bias.
          for (uint32_t c = 0; c < 3; ++c)
            exterior[c] += receiver.normal[c] * (.001f / source.sceneScale);
          receiver.cluster = source.partition->cluster(exterior.data());
        }
        receiver.surface = atlasIrradiance(layout, pixels, receiver.chart, coordinate, receiver.normal);
        if (const auto value = sample(grid, receiver.position, receiver.normal)) {
          receiver.covered = true;
          receiver.probe = *value;
        }
        result.push_back(receiver);
      }
    }
    return result;
  }
  void dumpReceiverReport(const scene::BakeScene &source, const lightmap::Layout &layout,
                          std::span<const uint16_t> pixels, const Grid &grid,
                          std::stop_token cancelled) noexcept {
    const char *path = std::getenv("REMIX_DEBUG_BAKE_RECEIVERS");
    if (!path || !*path) return;
    try {
      if (std::filesystem::exists(path))
        throw std::runtime_error("Receiver report exists; choose a fresh path");
      const auto receivers = compareReceivers(source, layout, pixels, grid, 8192, cancelled);
      uint32_t covered = 0;
      double absolute = 0, squared = 0, surface = 0, probe = 0;
      for (const auto &r : receivers) {
        if (!r.covered) continue;
        ++covered;
        const auto a = luminance(r.surface), b = luminance(r.probe);
        surface += a;
        probe += b;
        absolute += std::abs(a - b);
        squared += (a - b) * (a - b);
      }
      std::ofstream output(path);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output.precision(9);
      const double denominator = std::max(covered, 1u);
      output << "{\"version\":1,\"sceneHash\":\"" << source.contentHash
             << "\",\"scope\":\"Triangle-centre indirect irradiance at shared positions/vertex normals; published probe bias applies; no normal maps, materials or direct lights; triangle-weighted, not area-weighted\","
             << "\"sampled\":" << receivers.size() << ",\"covered\":" << covered
             << ",\"probeSpacingWorldUnits\":" << grid.header.originSpacing[3]
             << ",\"meanSurfaceLuminance\":" << surface / denominator
             << ",\"meanProbeLuminance\":" << probe / denominator
             << ",\"meanAbsoluteLuminanceError\":" << absolute / denominator
             << ",\"rmsLuminanceError\":" << std::sqrt(squared / denominator)
             << ",\"receivers\":[\n";
      for (size_t i = 0; i < receivers.size(); ++i) {
        const auto &r = receivers[i];
        output << (i ? ",\n" : "") << "{\"chart\":" << r.chart << ",\"draw\":" << r.draw
               << ",\"cluster\":";
        if (r.cluster == -2) output << "null";
        else output << r.cluster;
        output << ",\"position\":";
        vector(output, r.position);
        output << ",\"normal\":";
        vector(output, r.normal);
        output << ",\"surface\":";
        vector(output, r.surface);
        output << ",\"covered\":" << (r.covered ? "true" : "false") << ",\"probe\":";
        vector(output, r.probe);
        output << '}';
      }
      output << "\n]}\n";
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Receiver lighting report unavailable: %s\n", error.what());
    }
  }
} // namespace remix::forward::bake
