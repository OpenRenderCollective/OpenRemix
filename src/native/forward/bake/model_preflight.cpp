#include "forward/bake/model_preflight.h"
#include "forward/bake/model_triangles.h"
#include <fstream>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace remix::forward::bake {
  void writeModelPreflight(const scene::BakeScene &source, const std::filesystem::path &path,
                           std::stop_token cancelled) {
    const auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Model preflight superseded");
    };
    check();
    if (std::filesystem::exists(path)) throw std::runtime_error("Model preflight output already exists");
    if (source.draws.size() > 32768) throw std::length_error("Model preflight draw budget exceeded");
    struct Use {
      uint64_t triangles = 0;
      double area = 0;
      uint64_t vertices = 0;
      uint32_t group = 0;
    };
    std::vector<Use> uses(source.draws.size());
    uint64_t triangles = 0, vertices = 0;
    double area = 0;
    uint32_t currentDraw = ~0u;
    std::set<int64_t> currentVertices;
    forEachModelTriangle(source, [&](const ModelTriangle &triangle) {
      if (++triangles > 4000000) throw std::length_error("Model preflight triangle budget exceeded");
      if (currentDraw != triangle.draw) {
        if (currentDraw != ~0u) uses[currentDraw].vertices = currentVertices.size();
        currentVertices.clear();
        currentDraw = triangle.draw;
      }
      auto &use = uses[triangle.draw];
      ++use.triangles; use.area += triangle.areaSquareMetres; area += triangle.areaSquareMetres;
      const auto &draw = source.draws[triangle.draw];
      for (uint32_t c = 0; c < 3; ++c)
        if (currentVertices.insert(int64_t(draw.surface->indices[triangle.firstIndex + c]) + draw.vertexOffset).second) {
          ++vertices;
          if (currentVertices.size() > 2000000) throw std::length_error("Model preflight per-draw vertex budget exceeded");
        }
    }, cancelled);
    if (currentDraw != ~0u) uses[currentDraw].vertices = currentVertices.size();
    std::unordered_map<const scene::Surface *, uint32_t> surfaces;
    using Geometry = std::tuple<uint32_t, uint32_t, size_t, int32_t>;
    std::map<Geometry, uint32_t> groups;
    uint32_t draws = 0;
    for (uint32_t i = 0; i < uses.size(); ++i) {
      check();
      if (!uses[i].triangles) continue;
      ++draws;
      const auto &draw = source.draws[i];
      const auto [surface, added] = surfaces.emplace(draw.surface.get(), uint32_t(surfaces.size()));
      const size_t count = draw.indexCount ? draw.indexCount : draw.surface->indices.size();
      const auto [group, inserted] = groups.emplace(Geometry{surface->second, draw.firstIndex, count, draw.vertexOffset},
                                                  uint32_t(groups.size()));
      uses[i].group = group->second;
    }
    std::ostringstream text;
    text.imbue(std::locale::classic()); text.precision(12);
    text << "{\"version\":1,\"sceneHash\":\"" << source.contentHash << "\",\"sceneScale\":" << source.sceneScale
         << ",\"sourceDraws\":" << source.draws.size() << ",\"authoredCharts\":" << source.charts.size()
         << ",\"acceptedDraws\":" << draws << ",\"acceptedTriangles\":" << triangles
         << ",\"sourceVertexReferences\":" << vertices << ",\"areaSquareMetres\":" << area
         << ",\"rawGeometryGroups\":" << groups.size() << ",\"potentialRepeatedDraws\":" << draws - groups.size()
         << ",\"scope\":\"Eligibility only; no chart allocation, packing, lighting or GPU bake. "
            "Source vertex IDs count once per draw. Area is full admitted triangle area, not clipped visible area. "
            "Shared raw geometry does not prove chart or visibility reuse.\",\"draws\":[";
    bool comma = false;
    for (uint32_t i = 0; i < uses.size(); ++i) {
      check();
      const auto &use = uses[i];
      if (!use.triangles) continue;
      if (comma) text << ',';
      comma = true;
      const auto &draw = source.draws[i];
      text << "{\"draw\":" << i << ",\"triangles\":" << use.triangles << ",\"sourceVertices\":" << use.vertices
           << ",\"areaSquareMetres\":" << use.area << ",\"rawGeometryGroup\":" << use.group
           << ",\"rawDrawTriangles\":" << (draw.indexCount ? draw.indexCount : draw.surface->indices.size()) / 3 << '}';
    }
    text << "]}\n";
    check();
    if (std::filesystem::exists(path)) throw std::runtime_error("Model preflight output already exists");
    std::ofstream output(path);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << text.str(); output.close();
  }
}
