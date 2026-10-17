#include "forward/lightmap/data.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>

namespace remix::forward::lightmap {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface lightmap data preparation superseded");
    }
    uint16_t half(float value) {
      if (!std::isfinite(value))
        throw std::invalid_argument("Nonfinite surface lightmap coefficient");
      const uint32_t sign = std::bit_cast<uint32_t>(value) >> 16 & 0x8000u;
      const float magnitude = std::min(std::abs(value), 65000.f);
      if (magnitude < 6.103515625e-5f)
        return uint16_t(sign | uint32_t(std::lround(magnitude / 5.960464477539063e-8f)));
      const uint32_t bits = std::bit_cast<uint32_t>(magnitude) + 0x1000u;
      return uint16_t(sign | (((bits >> 23) - 112) << 10) | (bits >> 13 & 0x3ffu));
    }
  } // namespace
  std::vector<Mesh> meshes(const scene::BakeScene &scene, const Layout &layout, std::stop_token cancelled,
                           std::span<const bake::UInt4> lightChannels,
                           std::span<const bake::UInt4> directChannels) {
    if (layout.charts.size() != scene.charts.size() ||
        (!layout.mappings.empty() && layout.mappings.size() != scene.charts.size()) ||
        (!lightChannels.empty() && lightChannels.size() != scene.charts.size()) ||
        (!directChannels.empty() && directChannels.size() != scene.charts.size()))
      throw std::invalid_argument("Lightmap chart allocation mismatch");
    std::map<uint32_t, std::vector<uint32_t>> byDraw;
    for (uint32_t chart = 0; chart < scene.charts.size(); ++chart)
      byDraw[scene.charts[chart].draw].push_back(chart);
    std::vector<Mesh> result;
    for (auto &[drawIndex, charts] : byDraw) {
      check(cancelled);
      const auto &draw = scene.draws.at(drawIndex);
      const auto &surface = *draw.surface;
      const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
      std::vector<Vec4> coordinates(count);
      std::vector<Vec4> bounds(count);
      std::vector<bake::UInt4> lightKeys(count, {~0u, ~0u, ~0u, ~0u});
      std::vector<bake::UInt4> directKeys(count, {~0u, ~0u, ~0u, ~0u});
      for (uint32_t index : charts) {
        const auto &chart = scene.charts[index];
        const auto &allocation = layout.charts[index];
        if (!layout.mappings.empty() && layout.mappings[index].texcoords.size() != chart.texcoords.size())
          throw std::invalid_argument("Incomplete lightmap parameterization");
        for (size_t corner = 0; corner < chart.texcoords.size(); ++corner) {
          auto uv = chart.texcoords[corner];
          if (layout.mappings.size() == scene.charts.size())
            uv = layout.mappings[index].texcoords[corner];
          else {
            uv[0] *= float(allocation.width - 1) / float(chart.width - 1);
            uv[1] *= float(allocation.height - 1) / float(chart.height - 1);
          }
          coordinates[chart.firstIndex - draw.firstIndex + corner] = {
              (allocation.x + uv[0] + .5f) / layout.pageSize, (allocation.y + uv[1] + .5f) / layout.pageSize,
              float(allocation.page), float(index + 1)};
          bounds[chart.firstIndex - draw.firstIndex + corner] = {
              (allocation.x + .5f) / layout.pageSize, (allocation.y + .5f) / layout.pageSize,
              (allocation.x + allocation.width - .5f) / layout.pageSize,
              (allocation.y + allocation.height - .5f) / layout.pageSize};
          if (!lightChannels.empty())
            lightKeys[chart.firstIndex - draw.firstIndex + corner] = lightChannels[index];
          if (!directChannels.empty())
            directKeys[chart.firstIndex - draw.firstIndex + corner] = directChannels[index];
        }
      }
      Mesh mesh;
      mesh.draw = drawIndex;
      std::map<std::array<uint32_t, 13>, uint32_t> vertices;
      for (size_t corner = 0; corner < count; ++corner) {
        if ((corner & 1023u) == 0)
          check(cancelled);
        const auto vertex = uint32_t(int64_t(surface.indices[draw.firstIndex + corner]) + draw.vertexOffset);
        const auto coordinate = coordinates[corner];
        const std::array<uint32_t, 13> key = {vertex,
                                              std::bit_cast<uint32_t>(coordinate[0]),
                                              std::bit_cast<uint32_t>(coordinate[1]),
                                              std::bit_cast<uint32_t>(coordinate[2]),
                                              std::bit_cast<uint32_t>(coordinate[3]),
                                              lightKeys[corner][0],
                                              lightKeys[corner][1],
                                              lightKeys[corner][2],
                                              lightKeys[corner][3],
                                              directKeys[corner][0],
                                              directKeys[corner][1],
                                              directKeys[corner][2],
                                              directKeys[corner][3]};
        const auto [found, inserted] = vertices.emplace(key, uint32_t(mesh.vertices.size()));
        if (inserted)
          mesh.vertices.push_back(
              {surface.vertices[vertex], coordinate, lightKeys[corner], directKeys[corner], bounds[corner]});
        mesh.indices.push_back(found->second);
      }
      result.push_back(std::move(mesh));
    }
    return result;
  }
  std::vector<uint16_t> pixels(const Layout &layout, std::span<const bake::Result> results,
                               std::stop_token cancelled, std::span<const Vec4> visibility,
                               std::span<const Direct> direct, std::span<const Sun> sun) {
    if (!visibility.empty() && visibility.size() != results.size())
      throw std::invalid_argument("Incomplete direct visibility results");
    if (!direct.empty() && direct.size() != results.size())
      throw std::invalid_argument("Incomplete per-light direct irradiance results");
    if (!sun.empty() && sun.size() != results.size())
      throw std::invalid_argument("Incomplete surface sunlight results");
    if (results.size() != layout.points.size())
      throw std::invalid_argument("Incomplete surface lightmap results");
    std::vector<uint16_t> result(layout.owners.size() * atlasLayers * 4);
    const size_t pageTexels = size_t(layout.pageSize) * layout.pageSize;
    for (size_t texel = 0; texel < layout.owners.size(); ++texel) {
      if ((texel & 1023u) == 0)
        check(cancelled);
      const auto owner = layout.owners[texel];
      if (owner == unowned)
        continue;
      if (owner >= results.size())
        throw std::invalid_argument("Invalid surface lightmap donor");
      const size_t page = texel / pageTexels, pixel = texel % pageTexels;
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        for (uint32_t channel = 0; channel < 3; ++channel)
          result[((page * atlasLayers + coefficient) * pageTexels + pixel) * 4 + channel] =
              half(results[owner].irradiance[coefficient][channel]);
      for (uint32_t channel = 0; channel < 4; ++channel)
        result[((page * atlasLayers + irradianceLayers) * pageTexels + pixel) * 4 + channel] =
            half(visibility.empty() ? 1.f : std::clamp(visibility[owner][channel], 0.f, 1.f));
      result[((page * atlasLayers + sunVisibilityLayer) * pageTexels + pixel) * 4] =
          half(sun.empty() ? 1.f : std::clamp(sun[owner].visibility, 0.f, 1.f));
      if (!sun.empty())
        for (uint32_t coefficient = 0; coefficient < irradianceLayers; ++coefficient)
          result[((page * atlasLayers + coefficient) * pageTexels + pixel) * 4 + 3] =
              half(sun[owner].direct[coefficient]);
      if (!direct.empty())
        for (uint32_t coefficient = 0; coefficient < irradianceLayers; ++coefficient)
          for (uint32_t channel = 0; channel < 4; ++channel)
            result[((page * atlasLayers + directLayer + coefficient) * pageTexels + pixel) * 4 + channel] =
                half(direct[owner][coefficient][channel]);
    }
    return result;
  }
} // namespace remix::forward::lightmap
