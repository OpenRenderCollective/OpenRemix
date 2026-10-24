#include "forward/bake/model_receivers.h"
#include "forward/bake/model_charts.h"
#include "forward/lightmap/layout.h"
#include "forward/bake/probe_receiver_geometry.h"
#include "forward/bake/probe_receiver_report.h"
#include "scene/bake_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace b = remix::forward::bake;
namespace s = remix::scene;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  s::Draw triangle(float z = 0, remix::forward::Vec3 normal = {0, 0, 1}) {
    s::Draw draw;
    draw.surface = std::make_shared<s::Surface>();
    const float points[][3] = {{0, 0, z}, {3, 0, z}, {0, 3, z}};
    for (const auto &p : points) {
      s::Vertex v{};
      std::copy_n(p, 3, v.position);
      std::copy(normal.begin(), normal.end(), v.normal);
      draw.surface->vertices.push_back(v);
    }
    draw.surface->indices = {0, 1, 2};
    return draw;
  }
  template<class F> void invalid(F call) {
    try { call(); } catch (const std::invalid_argument &) { return; }
    require(false, "Invalid model receiver request accepted");
  }
}
int main(int argc, char **argv) {
  {
    s::BakeScene candidate;
    candidate.ready = true;
    candidate.draws = {triangle(), triangle(2)};
    for (auto &draw : candidate.draws) draw.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    auto &draw = candidate.draws[0];
    draw.surface->indices = {99, 99, 99, 1, 2, 3};
    draw.firstIndex = 3; draw.indexCount = 3; draw.vertexOffset = -1;
    draw.model[0] = 2; draw.model[5] = 3; draw.model[12] = 10;
    candidate.charts = {{1, 0, 2, 2, {{0, 0}, {1, 0}, {0, 1}}}};
    auto charts = b::modelCharts(candidate, 8);
    require(charts.size() == 1 && charts[0].draw == 0 && charts[0].firstIndex == 3 &&
                candidate.charts.size() == 1 && candidate.charts[0].draw == 1,
            "Derived model charts preserve signed ranges and caller-authored charts");
    candidate.charts.insert(candidate.charts.end(), charts.begin(), charts.end());
    s::validateBakeCharts(candidate.draws, candidate.charts);
    remix::forward::lightmap::Options options;
    options.pageSize = 64; options.maxPages = 1; options.maxTexels = 4096; options.fillBudget = true;
    const auto layout = remix::forward::lightmap::fit(candidate, options);
    require(layout.charts.size() == 2 && layout.pages == 1 && !layout.points.empty(),
            "Derived triangles fit through the existing shared world-metric atlas");
    bool generatedReceivers = false;
    for (size_t i = 0; i < layout.points.size(); ++i)
      if (layout.pointCharts[i] == 1 && layout.receiverSamples[i]) {
        const auto &p = layout.points[i].position;
        require(p[0] >= 9.99f && p[0] <= 16.01f && p[1] >= -.01f && p[1] <= 9.01f &&
                    std::abs(p[2]) < .01f,
                "Generated surface receivers use transformed model positions");
        generatedReceivers = true;
      }
    require(generatedReceivers, "Generated charts produce actual surface receivers");
    candidate.charts.clear();
    candidate.draws[0].classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    require(b::modelCharts(candidate, 8).size() == 1, "Dynamic geometry remains on volume lighting");
    candidate.draws[0].classification = REMIXAPI_INSTANCE_STATIC_EXT;
    try { b::modelCharts(candidate, 1); require(false, "Chart cap silently omitted static surfaces"); }
    catch (const std::length_error &) {}
    invalid([&] { b::modelCharts(candidate, 0); });
    std::stop_source stopped;
    stopped.request_stop();
    try { b::modelCharts(candidate, 8, stopped.get_token()); require(false, "Cancelled chart generation continued"); }
    catch (const std::runtime_error &) {}
    candidate.draws[1].surface->indices = {0, 0, 0};
    require(b::modelCharts(candidate, 1).size() == 1, "Degenerate faces consume no model chart budget");
    candidate.draws[0].surface->indices[3] = 999;
    invalid([&] { b::modelCharts(candidate, 8); });
  }
  s::BakeScene source;
  source.draws = {triangle()};
  auto sites = b::modelReceivers(source);
  require(sites.size() == 1 && sites[0].point.position == remix::forward::Vec4{1, 1, 0, 0} &&
              sites[0].point.normal == remix::forward::Vec4{0, 0, 1, 0},
          "Uncharted rigid geometry supplies a surface point and normal");
  auto &draw = source.draws[0];
  draw.model[0] = 2; draw.model[5] = 3; draw.model[10] = 4;
  draw.model[12] = 100000; draw.model[14] = 10;
  for (auto &vertex : draw.surface->vertices) vertex.normal[0] = vertex.normal[2] = 1;
  draw.surface->indices = {99, 99, 99, 1, 2, 3};
  draw.firstIndex = 3; draw.indexCount = 3; draw.vertexOffset = -1;
  sites = b::modelReceivers(source);
  require(sites.size() == 1 && sites[0].firstIndex == 3 &&
              sites[0].point.position == remix::forward::Vec4{100002, 3, 10, 0} &&
              std::abs(sites[0].point.normal[0] - 2/std::sqrt(5.f)) < 1e-6f &&
              std::abs(sites[0].point.normal[2] - 1/std::sqrt(5.f)) < 1e-6f,
          "Model samples preserve subranges, signed offsets and inverse-transpose normals");
  draw.material = std::make_shared<s::Material>();
  draw.material->transmission = true;
  require(b::modelReceivers(source).empty(), "Transmissive geometry is outside opaque support");
  draw.material->transmission = false; draw.material->compare = 5;
  require(b::modelReceivers(source).empty(), "Alpha coverage is not guessed from triangle geometry");
  draw.ignoreAlpha = true;
  require(b::modelReceivers(source).size() == 1, "Explicit opaque alpha override remains eligible");
  for (auto flag : {&draw.decal, &draw.viewModel, &draw.reflectionOnly, &draw.wireframe}) {
    *flag = true;
    require(b::modelReceivers(source).empty(), "Overlay and nonreceiver geometry is excluded");
    *flag = false;
  }
  draw.blend.enabled = true;
  require(b::modelReceivers(source).empty(), "Blended receivers need a separate coverage model");

  source.draws = {triangle()};
  auto &surface = *source.draws[0].surface;
  for (uint32_t i = 0; i < 3; ++i) { auto vertex = surface.vertices[i]; vertex.position[2] = 5; surface.vertices.push_back(vertex); }
  surface.indices.insert(surface.indices.end(), {3, 4, 5});
  source.charts = {{0, 0, 2, 2, {{0, 0}, {1, 0}, {0, 1}}}};
  sites = b::modelReceivers(source);
  require(sites.size() == 1 && sites[0].firstIndex == 3 && sites[0].point.position[2] == 5,
          "Partially charted draws retain their uncharted triangles only");
  source.charts.push_back({0, 3, 2, 2, {{0, 0}, {1, 0}, {0, 1}}});
  require(b::modelReceivers(source).empty(), "Fully lightmapped geometry consumes no model sample budget");
  source.charts.push_back(source.charts[0]);
  invalid([&] { b::modelReceivers(source); });

  // Shared range enumeration must preserve holes on both sides of authored coverage.
  source = {};
  source.draws = {triangle(), triangle()};
  source.draws[0].surface->indices = {0, 1, 2, 0, 1, 2, 0, 1, 2};
  source.charts = {{0, 3, 2, 2, {{0, 0}, {1, 0}, {0, 1}}}};
  auto ranges = b::unchartedReceiverRanges(source);
  require(ranges.size() == 3 && ranges[0].draw == 0 && ranges[0].firstIndex == 0 &&
              ranges[0].indexCount == 3 && ranges[1].draw == 0 && ranges[1].firstIndex == 6 &&
              ranges[1].indexCount == 3 && ranges[2].draw == 1 && ranges[2].indexCount == 3,
          "Authored chart splits uncharted geometry into exact disjoint ranges");
  source.draws[0].viewModel = true;
  ranges = b::unchartedReceiverRanges(source);
  require(ranges.size() == 1 && ranges[0].draw == 1,
          "Excluded charted draw does not hide later uncharted receivers");
  source.draws[0].viewModel = false;
  source.charts[0].firstIndex = 1;
  invalid([&] { b::unchartedReceiverRanges(source); });
  source.charts[0].firstIndex = 9;
  invalid([&] { b::unchartedReceiverRanges(source); });

  source = {};
  auto directional = triangle();
  directional.surface->vertices.clear(); directional.surface->indices.clear();
  for (uint32_t face = 0; face < 6; ++face) {
    remix::forward::Vec3 normal{};
    normal[face / 2] = face & 1 ? -1.f : 1.f;
    const auto part = triangle(float(face), normal);
    const auto first = uint32_t(directional.surface->vertices.size());
    directional.surface->vertices.insert(directional.surface->vertices.end(), part.surface->vertices.begin(), part.surface->vertices.end());
    directional.surface->indices.insert(directional.surface->indices.end(), {first, first + 1, first + 2});
  }
  source.draws = {directional};
  sites = b::modelReceivers(source);
  require(sites.size() == 6, "Representative model support retains all six normal directions");
  auto larger = triangle(10, {1, 0, 0});
  for (auto &vertex : larger.surface->vertices) { vertex.position[0] *= 2; vertex.position[1] *= 2; }
  const auto largerFirst = uint32_t(directional.surface->vertices.size());
  directional.surface->vertices.insert(directional.surface->vertices.end(), larger.surface->vertices.begin(), larger.surface->vertices.end());
  directional.surface->indices.insert(directional.surface->indices.end(), {largerFirst, largerFirst + 1, largerFirst + 2});
  sites = b::modelReceivers(source);
  require(sites.size() == 6 && sites[0].firstIndex == 18 && sites[0].point.position[2] == 10,
          "Each normal direction retains its largest world-space triangle rather than its first triangle");
  source.draws.push_back(directional);
  sites = b::modelReceivers(source, 4);
  require(sites.size() == 4 && sites[0].draw == 0 && sites[1].draw == 1 && sites[2].draw == 0 && sites[3].draw == 1,
          "Budget rounds give every selected draw a sample before adding more faces");

  source = {};
  for (uint32_t i = 0; i < 20; ++i) source.draws.push_back(triangle(float(i)));
  sites = b::modelReceivers(source, 3);
  require(sites.size() == 3 && sites[0].draw == 3 && sites[1].draw == 10 && sites[2].draw == 16,
          "Small budgets span the eligible draw set instead of only its first models");
  source.draws = {triangle(0, {})};
  require(b::modelReceivers(source)[0].point.normal == remix::forward::Vec4{0, 0, 1, 0},
          "Missing vertex normals use the world-space geometric normal");
  source.draws[0].surface->vertices[2].position[1] = 0;
  require(b::modelReceivers(source).empty(), "Degenerate triangles contribute no sample");
  source.draws = {triangle(-1), triangle(1)};
  auto partition = std::make_shared<s::VisibilityPartition>();
  partition->closed = true; partition->clusterCount = 1;
  partition->nodes = {{{0, 0, 1, 0}, {-1, -2}, {}}};
  partition->leafClusters = {0, -1};
  source.partition = partition;
  sites = b::modelReceivers(source);
  require(sites.size() == 1 && sites[0].draw == 1,
          "Geometry buried in a closed partition does not consume lighting support samples");
  for (auto &draw : source.draws) draw.classification = REMIXAPI_INSTANCE_STATIC_EXT;
  auto partitionCharts = b::modelCharts(source, 8);
  require(partitionCharts.size() == 1 && partitionCharts[0].draw == 1,
          "Solid-partition model faces consume no chart space");
  source.partition.reset();
  source.draws = {triangle()};
  source.draws[0].surface->indices[0] = 99;
  invalid([&] { b::modelReceivers(source); });
  invalid([&] { b::modelReceivers(source, 0); });
  invalid([&] { b::modelReceivers(source, 4097); });
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try { b::modelReceivers(source, 4096, stop.get_token()); } catch (const std::runtime_error &) { cancelled = true; }
  require(cancelled, "Superseded geometry sampling stops before allocation");

  require(argc == 2, "Supply an isolated manifest output path");
  const std::filesystem::path path(argv[1]);
  require(!std::filesystem::exists(path), "Model manifest fixture already exists");
  const char *key = "REMIX_DEBUG_BAKE_MODEL_RECEIVERS";
  const auto previous = std::getenv(key) ? std::optional<std::string>(std::getenv(key)) : std::nullopt;
  auto environment = [&](const char *value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value) setenv(key, value, 1); else unsetenv(key);
#endif
  };
  b::Grid grid;
  grid.header.originSpacing = {-1, -1, -1, 10};
  grid.header.dimensions = {2, 2, 2, 8};
  grid.probes.resize(8);
  for (auto &probe : grid.probes) {
    probe.statistics[3] = 1;
    probe.irradiance[0] = {1, 1, 1, 0};
    for (auto &moment : probe.moments) moment = {1000, 1000000, 0, 1};
  }
  source.draws = {triangle()}; source.contentHash = 1234;
  environment(nullptr);
  b::dumpModelReceiverReport(source, grid);
  require(!std::filesystem::exists(path), "Manifest allocation is opt-in");
  environment(path.string().c_str());
  b::dumpModelReceiverReport(source, grid);
  auto read = [&] { std::ifstream file(path); return std::string(std::istreambuf_iterator<char>(file), {}); };
  const auto original = read();
  require(original.find("\"sceneHash\":\"1234\"") != std::string::npos &&
              original.find("\"position\":[1,1,0]") != std::string::npos &&
              original.find("\"probe\":null") == std::string::npos,
          "Manifest preserves source identity, geometry and current field estimate");
  b::dumpModelReceiverReport(source, grid);
  require(read() == original, "Existing diagnostic evidence is protected");
  environment(previous ? previous->c_str() : nullptr);
  std::filesystem::remove(path);
}
