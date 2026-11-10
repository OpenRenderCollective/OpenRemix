#include "forward/lightmap/data.h"
#include "forward/half.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace remix::forward::lightmap {
  bool fullyMapped(const Mesh &mesh) {
    return !mesh.indices.empty() && std::all_of(mesh.indices.begin(), mesh.indices.end(), [&](uint32_t index) {
      return mesh.vertices.at(index).coordinate[3] >= .5f;
    });
  }
  std::vector<uint32_t> directOwners(const Mesh &mesh) {
    std::unordered_set<uint32_t> unique;
    unique.reserve(std::min<size_t>(mesh.vertices.size(), 64));
    for (const auto &vertex : mesh.vertices)
      for (const auto light : vertex.directLights)
        if (light != ~0u) unique.insert(light);
    std::vector<uint32_t> owners(unique.begin(), unique.end());
    std::sort(owners.begin(), owners.end());
    return owners;
  }
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface lightmap data preparation superseded");
    }
  } // namespace
  std::vector<ReceiverRun> receiverRuns(const Mesh &mesh, std::stop_token cancelled) {
    if (mesh.indices.size() % 3 || mesh.indices.size() > UINT32_MAX)
      throw std::invalid_argument("Receiver runs require bounded triangle-list indices");
    std::vector<ReceiverRun> result;
    for (uint32_t first = 0; first < mesh.indices.size(); first += 3) {
      if ((first & 1023u) == 0) check(cancelled);
      uint32_t mapped = 0;
      for (uint32_t corner = 0; corner < 3; ++corner)
        mapped += mesh.vertices.at(mesh.indices[first + corner]).coordinate[3] >= .5f;
      const auto coverage = mapped == 3 ? ReceiverCoverage::Mapped
                          : mapped == 0 ? ReceiverCoverage::Unmapped : ReceiverCoverage::Mixed;
      if (result.empty() || result.back().coverage != coverage) {
        if (result.size() == 16) return {};
        result.push_back({first, 3, coverage});
      } else
        result.back().indexCount += 3;
    }
    return result;
  }
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
              half::encode(results[owner].irradiance[coefficient][channel]);
      for (uint32_t channel = 0; channel < 4; ++channel)
        result[((page * atlasLayers + irradianceLayers) * pageTexels + pixel) * 4 + channel] =
            half::encode(visibility.empty() ? 1.f : std::clamp(visibility[owner][channel], 0.f, 1.f));
      result[((page * atlasLayers + sunVisibilityLayer) * pageTexels + pixel) * 4] =
          half::encode(sun.empty() ? 1.f : std::clamp(sun[owner].visibility, 0.f, 1.f));
      if (!sun.empty())
        for (uint32_t coefficient = 0; coefficient < irradianceLayers; ++coefficient)
          result[((page * atlasLayers + coefficient) * pageTexels + pixel) * 4 + 3] =
              half::encode(sun[owner].direct[coefficient]);
      if (!direct.empty())
        for (uint32_t coefficient = 0; coefficient < irradianceLayers; ++coefficient)
          for (uint32_t channel = 0; channel < 4; ++channel)
            result[((page * atlasLayers + directLayer + coefficient) * pageTexels + pixel) * 4 + channel] =
                half::encode(direct[owner][coefficient][channel]);
    }
    return result;
  }
} // namespace remix::forward::lightmap
