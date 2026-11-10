#include "forward/bake/model_chart_report.h"
#include "forward/material.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

namespace remix::forward::bake {
  void writeModelChartMesh(const scene::BakeScene &source, uint32_t index,
      const std::filesystem::path &path, std::stop_token cancelled) {
    const auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Model mesh export superseded");
    };
    check();
    if (std::filesystem::exists(path)) throw std::runtime_error("Model mesh output already exists");
    const auto &draw = source.draws.at(index);
    if (!draw.surface || !std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid model mesh export source");
    const auto &surface = *draw.surface;
    const size_t count = draw.indexCount ? draw.indexCount : surface.indices.size();
    if (!count || count % 3 || count > 32768u * 3 || draw.firstIndex > surface.indices.size() ||
        count > surface.indices.size() - draw.firstIndex)
      throw std::invalid_argument("Model mesh export requires 1..32768 triangles");
    std::ostringstream text;
    text.imbue(std::locale::classic()); text.precision(10);
    text << "# OpenRemix frozen model mesh; scene " << source.contentHash << "; draw " << index
         << "; positions in metres relative to first corner\n";
    const auto normals = normalMatrix(draw.model);
    Vec3 origin{};
    std::map<int64_t, uint32_t> vertices;
    std::vector<uint32_t> corners;
    corners.reserve(count);
    for (size_t corner = 0; corner < count; ++corner) {
      check();
      const auto vertex = int64_t(surface.indices[draw.firstIndex + corner]) + draw.vertexOffset;
      if (vertex < 0 || uint64_t(vertex) >= surface.vertices.size())
        throw std::invalid_argument("Invalid model export vertex index");
      const auto [found, inserted] = vertices.try_emplace(vertex, uint32_t(vertices.size()) + 1);
      corners.push_back(found->second);
      if (!inserted) continue;
      const auto &v = surface.vertices[size_t(vertex)];
      const auto p = transformPoint(draw.model, {v.position[0], v.position[1], v.position[2]});
      auto n = transformVector(normals, {v.normal[0], v.normal[1], v.normal[2]});
      const double length = std::hypot(double(n[0]), double(n[1]), double(n[2]));
      if (length > 0) for (auto &value : n) value = float(value / length);
      for (uint32_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(p[axis]) || !std::isfinite(n[axis])) throw std::invalid_argument("Nonfinite model export");
      if (!std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1])) throw std::invalid_argument("Nonfinite model export UV");
      if (!corner) {
        origin = p;
        text << "# origin_host " << origin[0] << ' ' << origin[1] << ' ' << origin[2]
             << "; scene_scale " << source.sceneScale << '\n';
      }
      text << "# source_vertex " << vertex << "\nv";
      for (uint32_t axis = 0; axis < 3; ++axis) text << ' ' << (double(p[axis]) - origin[axis]) * source.sceneScale;
      text << "\nvt " << v.uv[0] << ' ' << v.uv[1] << "\nvn " << n[0] << ' ' << n[1] << ' ' << n[2] << '\n';
    }
    for (size_t first = 0; first < count; first += 3) {
      text << 'f';
      for (size_t c = first; c < first + 3; ++c) {
        const auto index = corners[c];
        text << ' ' << index << '/' << index << '/' << index;
      }
      text << '\n';
    }
    check();
    if (std::filesystem::exists(path)) throw std::runtime_error("Model mesh output already exists");
    std::ofstream output(path);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << text.str();
    output.close();
  }
  std::string modelChartBudgetFailure(const scene::BakeScene &source,
      const std::vector<scene::BakeSurfaceChart> &charts, uint32_t draw, uint32_t first,
      std::stop_token cancelled) {
    struct Use { uint32_t draw = 0, charts = 0; uint64_t triangles = 0; };
    std::map<uint32_t, Use> uses;
    for (const auto &chart : charts) {
      auto &use = uses[chart.draw];
      use.draw = chart.draw; ++use.charts; use.triangles += chart.texcoords.size() / 3;
    }
    std::vector<Use> ranked;
    for (const auto &[index, use] : uses) ranked.push_back(use);
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
      return a.charts != b.charts ? a.charts > b.charts : a.draw < b.draw;
    });
    std::ostringstream out;
    out << "Model chart budget exhausted: " << charts.size() << " charts across " << uses.size()
        << " draws; stopped at draw " << draw << " index " << first << "; largest prefix consumers:";
    for (size_t i = 0; i < std::min<size_t>(8, ranked.size()); ++i) {
      const auto &use = ranked[i];
      const auto &owner = source.draws[use.draw];
      out << " [draw " << use.draw << ", charts " << use.charts << ", accepted triangles " << use.triangles
          << ", draw triangles " << (owner.indexCount ? owner.indexCount : owner.surface->indices.size()) / 3
          << ", material " << std::hex << (owner.material ? owner.material->sourceHash : 0) << std::dec << ']';
    }
    const char *path = std::getenv("REMIX_DEBUG_BAKE_MODEL_CHART_MESH");
    if (path && *path && !ranked.empty()) {
      for (size_t i = 0; i < std::min<size_t>(8, ranked.size()); ++i) {
        auto output = std::filesystem::u8path(path);
        if (i) output += ".draw-" + std::to_string(ranked[i].draw) + ".obj";
        try { writeModelChartMesh(source, ranked[i].draw, output, cancelled); }
        catch (const std::exception &error) {
          std::fprintf(stderr, "Model chart mesh report unavailable: %s\n", error.what());
        }
      }
    }
    return out.str();
  }

}
