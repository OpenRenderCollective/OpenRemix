#include "forward/lightmap/data.h"
#include "forward/bake/model_charts.h"
#include "scene/bake_scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace s = remix::scene;
namespace l = remix::forward::lightmap;
namespace {
  void require(bool value, const char *message) {
    if (!value) throw std::invalid_argument(message);
  }
  s::BakeScene readCharts(const std::filesystem::path &path, bool baseline) {
    require(std::filesystem::file_size(path) <= 16u << 20, "Chart study input exceeds 16 MiB");
    std::ifstream input(path);
    std::string magic;
    uint32_t vertices = 0, indices = 0, charts = 0;
    input >> magic >> vertices >> indices >> charts;
    require(magic == "MCHART1" && vertices && vertices <= 98304 && indices && indices % 3 == 0 &&
                indices <= 98304 && charts && charts <= indices / 3, "Invalid chart study counts");
    s::BakeScene scene;
    scene.ready = true;
    scene.draws.resize(1);
    auto &draw = scene.draws[0];
    draw.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    draw.surface = std::make_shared<s::Surface>();
    auto &surface = *draw.surface;
    surface.vertices.resize(vertices);
    for (auto &vertex : surface.vertices) {
      for (auto &value : vertex.position) input >> value;
      for (auto &value : vertex.normal) input >> value;
      for (auto &value : vertex.uv) input >> value;
      require(bool(input), "Incomplete study vertex");
      std::fill_n(vertex.color, 4, 1.f);
      for (auto value : vertex.position) require(std::isfinite(value), "Nonfinite study position");
      for (auto value : vertex.normal) require(std::isfinite(value), "Nonfinite study normal");
      for (auto value : vertex.uv) require(std::isfinite(value), "Nonfinite study material UV");
    }
    std::vector<uint32_t> original(indices);
    for (auto &index : original) { input >> index; require(index < vertices, "Invalid study vertex index"); }
    std::vector<uint8_t> owned(indices / 3);
    for (uint32_t i = 0; i < charts; ++i) {
      uint32_t count = 0; input >> count;
      require(count && count <= indices / 3, "Invalid study chart count");
      s::BakeSurfaceChart chart{0, uint32_t(surface.indices.size()), 2, 2, {}};
      std::array<float, 2> minimum{INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY};
      for (uint32_t triangle = 0; triangle < count; ++triangle) {
        uint32_t face = 0; input >> face;
        require(face < owned.size() && !owned[face], "Duplicate or invalid study face ownership");
        owned[face] = 1;
        for (uint32_t c = 0; c < 3; ++c) {
          std::array<float, 2> uv{};
          for (uint32_t axis = 0; axis < 2; ++axis) {
            input >> uv[axis]; require(bool(input) && std::isfinite(uv[axis]), "Invalid study chart UV");
            minimum[axis] = std::min(minimum[axis], uv[axis]);
            maximum[axis] = std::max(maximum[axis], uv[axis]);
          }
          chart.texcoords.push_back(uv);
          surface.indices.push_back(original[face * 3 + c]);
        }
      }
      for (uint32_t axis = 0; axis < 2; ++axis) {
        require(maximum[axis] > minimum[axis], "Collapsed study chart UV bounds");
        for (auto &uv : chart.texcoords) uv[axis] = (uv[axis] - minimum[axis]) / (maximum[axis] - minimum[axis]);
      }
      scene.charts.push_back(std::move(chart));
    }
    require(bool(input) && surface.indices.size() == indices &&
                std::all_of(owned.begin(), owned.end(), [](auto value) { return value == 1; }),
            "Incomplete study face coverage");
    input >> std::ws;
    require(input.eof(), "Trailing chart study input");
    if (baseline) {
      surface.indices = std::move(original);
      scene.charts.clear();
      scene.charts = remix::forward::bake::modelCharts(scene, 65536);
    }
    return scene;
  }
  s::BakeScene triangle() {
    s::BakeScene scene;
    scene.ready = true; scene.draws.resize(1);
    auto &draw = scene.draws[0];
    draw.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    draw.surface = std::make_shared<s::Surface>();
    draw.surface->vertices.resize(3);
    draw.surface->vertices[1].position[0] = 1;
    draw.surface->vertices[2].position[1] = 1;
    for (auto &v : draw.surface->vertices) v.normal[2] = 1;
    draw.surface->indices = {0, 1, 2};
    scene.charts = {{0, 0, 2, 2, {{0, 0}, {1, 0}, {0, 1}}}};
    return scene;
  }
}
int main(int argc, char **argv) try {
  require(argc <= 3 && (argc < 3 || std::string(argv[2]) == "--baseline"), "Supply chart data and optional --baseline");
  const auto scene = argc >= 2 ? readCharts(argv[1], argc == 3) : triangle();
  s::validateBakeCharts(scene.draws, scene.charts);
  l::Options options;
  options.padding = 0; options.pageSize = 1024; options.maxPages = 4;
  options.maxTexels = 4194304; options.luxelMetres = .248f;
  const auto layout = l::fit(scene, options);
  auto finalCharts = scene.charts;
  require(finalCharts.size() == layout.mappings.size(), "Study parameterization lost chart ownership");
  for (size_t i = 0; i < finalCharts.size(); ++i) {
    finalCharts[i].width = layout.mappings[i].width;
    finalCharts[i].height = layout.mappings[i].height;
    finalCharts[i].texcoords = layout.mappings[i].texcoords;
  }
  s::validateBakeCharts(scene.draws, finalCharts);
  const auto meshes = l::meshes(scene, layout);
  require(meshes.size() == 1 && l::fullyMapped(meshes[0]) && !layout.points.empty(),
          "Imported chart study lost receiver or retained-mesh coverage");
  uint64_t rectangles = 0;
  for (const auto &chart : layout.charts) rectangles += uint64_t(chart.width) * chart.height;
  std::cout << "{\"charts\":" << layout.charts.size() << ",\"receiverPoints\":" << layout.points.size()
            << ",\"rectangleTexels\":" << rectangles << ",\"pageSize\":" << layout.pageSize
            << ",\"pages\":" << layout.pages << ",\"spacingMetres\":" << layout.luxelMetres
            << ",\"fullyMapped\":true,\"finalParameterizationValidated\":true}\n";
  return 0;
} catch (const std::exception &error) {
  std::fprintf(stderr, "Model chart study failed: %s\n", error.what()); return 1;
} catch (remixapi_ErrorCode error) {
  std::fprintf(stderr, "Model chart validation failed: %u\n", unsigned(error)); return 1;
}
