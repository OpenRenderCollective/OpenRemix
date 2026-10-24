#include "forward/bake/model_receivers.h"
#include "forward/bake/model_charts.h"
#include "forward/bake/model_triangles.h"
#include "forward/bake/model_preflight.h"
#include "forward/bake/model_chart_report.h"
#include "forward/lightmap/layout.h"
#include "forward/lightmap/data.h"
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
int main(int argc, char **argv) try {
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
    catch (const std::length_error &error) {
      const std::string message(error.what());
      require(message.find("1 charts across 1 draws; stopped at draw 1 index 0") != std::string::npos &&
                  message.find("largest prefix consumers: [draw 0, charts 1, accepted triangles 1") != std::string::npos,
              "Chart cap reports bounded ownership and partial-progress provenance");
    }
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
  {
    s::BakeScene patch;
    patch.ready = true;
    patch.draws = {triangle()};
    auto &draw = patch.draws[0];
    draw.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    auto vertex = draw.surface->vertices[0];
    vertex.position[0] = vertex.position[1] = 3;
    draw.surface->vertices.push_back(vertex);
    draw.surface->indices = {0, 1, 2, 1, 3, 2};
    auto charts = b::modelCharts(patch, 1);
    require(charts.size() == 1 && charts[0].texcoords.size() == 6,
            "Connected quad shares one model chart within a one-chart budget");
    patch.charts = charts;
    s::validateBakeCharts(patch.draws, patch.charts);
    remix::forward::lightmap::Options options;
    options.pageSize = 64; options.maxPages = 1; options.maxTexels = 4096; options.fillBudget = true;
    require(!remix::forward::lightmap::fit(patch, options).points.empty(),
            "Joined model chart produces real surface samples");
    patch.charts.clear();
    draw.surface->vertices[3].position[2] = .5f;
    charts = b::modelCharts(patch, 1);
    require(charts.size() == 1 && charts[0].texcoords.size() == 6,
            "Smooth nonplanar connected triangles retain one injective chart");
    patch.charts = charts;
    const auto curvedLayout = remix::forward::lightmap::fit(patch, options);
    const auto curvedMeshes = remix::forward::lightmap::meshes(patch, curvedLayout);
    require(!curvedLayout.points.empty() && curvedMeshes.size() == 1 &&
                remix::forward::lightmap::fullyMapped(curvedMeshes[0]),
            "Connected curved model chart supplies complete retained-mesh lightmap bindings");
    patch.charts.clear();
    draw.surface->indices = {0, 1, 2, 0, 1, 2};
    require(b::modelCharts(patch, 2).size() == 2,
            "Overlapping triangles never merge into an invalid projected chart");
    draw.surface->indices = {0, 1, 2, 1, 2, 3};
    require(b::modelCharts(patch, 2).size() == 2,
            "Opposing triangle orientations remain separate charts");
  }
  {
    s::BakeScene clipped;
    auto partition = std::make_shared<s::VisibilityPartition>();
    partition->closed = true; partition->clusterCount = 1;
    partition->nodes = {{{1, 0, 0, 1}, {-1, 1}, {}}, {{1, 0, 0, -1}, {-2, -1}, {}}};
    partition->leafClusters = {-1, 0};
    clipped.partition = partition;
    using Triangle = std::array<std::array<float, 3>, 3>;
    const Triangle crossing{{{-4, -4, 0}, {-4, 4, 0}, {4, 0, 0}}};
    require(partition->cluster(std::array<float,3>{-4.f/3, 0, 0}.data()) < 0 &&
                b::playableReceiverTriangle(clipped, crossing),
            "Playable triangle fragments survive even when centroid and all corners are solid");
    require(!b::playableReceiverTriangle(clipped, Triangle{{{-4, -4, 0}, {-4, 4, 0}, {-3, 0, 0}}}),
            "Fully buried triangles still consume no model chart space");
    require(b::playableReceiverTriangle(clipped, Triangle{{{-4, -4, 0}, {-4, 4, 0}, {0, 0, 0}}}),
            "Visible corner prevents rejecting a triangle whose centre is solid");
    clipped.draws = {triangle()};
    clipped.draws[0].classification = REMIXAPI_INSTANCE_STATIC_EXT;
    for (uint32_t i = 0; i < 3; ++i)
      std::copy(crossing[i].begin(), crossing[i].end(), clipped.draws[0].surface->vertices[i].position);
    require(b::modelCharts(clipped, 8).size() == 1,
            "Partially playable model triangles receive an actual lightmap chart");
    std::stop_source stopped; stopped.request_stop();
    try { b::playableReceiverTriangle(clipped, crossing, stopped.get_token()); require(false, "Cancelled BSP clipping continued"); }
    catch (const std::runtime_error &) {}
    partition->nodes = {{{1, 0, 0, 0}, {0, 0}, {}}};
    require(b::playableReceiverTriangle(clipped, crossing),
            "Partition traversal budget preserves coverage instead of guessing outside");
    partition->nodes.clear();
    require(b::playableReceiverTriangle(clipped, crossing), "Missing partition topology is conservative");
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
  {
    s::BakeScene inventory;
    inventory.draws = {triangle()};
    inventory.draws[0].classification = REMIXAPI_INSTANCE_STATIC_EXT;
    inventory.draws.push_back(inventory.draws[0]);
    inventory.draws.push_back(inventory.draws[0]); inventory.draws[2].viewModel = true;
    uint32_t count = 0;
    double area = 0;
    b::forEachModelTriangle(inventory, [&](const b::ModelTriangle &triangle) {
      ++count; area += triangle.areaSquareMetres;
      require(triangle.firstIndex == 0 && triangle.positions[1][0] == 3,
              "Model triangle stream preserves indexed geometry");
    });
    require(count == 2 && area == 9, "Triangle inventory streams full eligible geometry independently of chart cap");
    try { b::modelCharts(inventory, 1); require(false, "Chart cap no longer rejects partial output"); }
    catch (const std::length_error &) {}
    auto report = path; report += ".preflight.json";
    std::filesystem::remove(report);
    b::writeModelPreflight(inventory, report);
    std::ifstream input(report);
    const std::string text(std::istreambuf_iterator<char>(input), {});
    input.close();
    require(text.find("\"acceptedDraws\":2,\"acceptedTriangles\":2,\"sourceVertexReferences\":6") != std::string::npos &&
                text.find("\"areaSquareMetres\":9,\"rawGeometryGroups\":1,\"potentialRepeatedDraws\":1") != std::string::npos,
            "Model inventory preserves per-draw counts and labels only potential raw geometry reuse");
    try { b::writeModelPreflight(inventory, report); require(false, "Model preflight overwrote existing output"); }
    catch (const std::runtime_error &) {}
    std::filesystem::remove(report);
    std::stop_source stopped; stopped.request_stop();
    try { b::writeModelPreflight(inventory, report, stopped.get_token()); require(false, "Cancelled preflight continued"); }
    catch (const std::runtime_error &) {}
    require(!std::filesystem::exists(report), "Cancelled preflight wrote output");
    invalid([&] { b::forEachModelTriangle(inventory, {}); });
    inventory.draws.resize(32769);
    try { b::writeModelPreflight(inventory, report); require(false, "Model preflight draw bound ignored"); }
    catch (const std::length_error &) {}
  }

  {
    auto meshPath = path; meshPath += ".mesh.obj";
    std::filesystem::remove(meshPath);
    s::BakeScene exported;
    exported.sceneScale = .5f; exported.draws = {triangle()};
    auto &draw = exported.draws[0];
    draw.model[0] = 2; draw.model[12] = 100;
    draw.surface->indices = {99, 99, 99, 1, 2, 3};
    draw.firstIndex = 3; draw.indexCount = 3; draw.vertexOffset = -1;
    draw.surface->vertices[0].uv[0] = .25f;
    b::writeModelChartMesh(exported, 0, meshPath);
    std::ifstream input(meshPath);
    const std::string mesh(std::istreambuf_iterator<char>(input), {});
    input.close();
    require(mesh.find("v 0 0 0\nvt 0.25 0") != std::string::npos &&
                mesh.find("v 3 0 0") != std::string::npos && mesh.find("v 0 1.5 0") != std::string::npos &&
                mesh.find("vn 0 0 1") != std::string::npos && mesh.find("f 1/1/1 2/2/2 3/3/3") != std::string::npos,
            "Offline mesh export preserves signed ranges, winding, material UVs and metric transforms");
    try { b::writeModelChartMesh(exported, 0, meshPath); require(false, "Mesh export overwrote an existing file"); }
    catch (const std::runtime_error &) {}
    std::filesystem::remove(meshPath);
    draw.surface->indices.insert(draw.surface->indices.end(), {1, 2, 3});
    draw.indexCount = 6;
    b::writeModelChartMesh(exported, 0, meshPath);
    std::ifstream sharedInput(meshPath);
    const std::string shared(std::istreambuf_iterator<char>(sharedInput), {});
    sharedInput.close();
    const auto face = shared.find("f 1/1/1 2/2/2 3/3/3");
    require(shared.find("f 1/1/1 2/2/2 3/3/3", face + 1) != std::string::npos &&
                shared.find("# source_vertex 0") != std::string::npos &&
                shared.find("# source_vertex 2") != std::string::npos &&
                shared.find("# source_vertex 3") == std::string::npos,
            "Offline mesh export preserves original shared-index connectivity");
    std::filesystem::remove(meshPath);
    std::stop_source stopped; stopped.request_stop();
    try { b::writeModelChartMesh(exported, 0, meshPath, stopped.get_token()); require(false, "Cancelled mesh export continued"); }
    catch (const std::runtime_error &) {}
    require(!std::filesystem::exists(meshPath), "Cancelled mesh export created output");
    draw.indexCount = 32769 * 3;
    invalid([&] { b::writeModelChartMesh(exported, 0, meshPath); });
    const char *oldMeshPath = std::getenv("REMIX_DEBUG_BAKE_MODEL_CHART_MESH");
    const std::optional<std::string> previousMeshPath = oldMeshPath ? std::optional<std::string>(oldMeshPath) : std::nullopt;
#ifdef _WIN32
    _putenv_s("REMIX_DEBUG_BAKE_MODEL_CHART_MESH", meshPath.string().c_str());
#else
    setenv("REMIX_DEBUG_BAKE_MODEL_CHART_MESH", meshPath.string().c_str(), 1);
#endif
    s::BakeScene many;
    for (uint32_t i = 0; i < 10; ++i) many.draws.push_back(triangle());
    std::vector<s::BakeSurfaceChart> ownership;
    for (uint32_t i = 0; i < 9; ++i) ownership.push_back({i, 0, 2, 2, {{0, 0}, {1, 0}, {0, 1}}});
    b::modelChartBudgetFailure(many, ownership, 9, 0);
    require(std::filesystem::exists(meshPath), "Requested budget-failure export is missing");
    std::filesystem::remove(meshPath);
    for (uint32_t i = 1; i <= 8; ++i) {
      auto extra = meshPath; extra += ".draw-" + std::to_string(i) + ".obj";
      require(std::filesystem::exists(extra) == (i < 8), "Budget-failure mesh corpus exceeds eight-draw bound");
      if (i < 8) std::filesystem::remove(extra);
    }
#ifdef _WIN32
    _putenv_s("REMIX_DEBUG_BAKE_MODEL_CHART_MESH", previousMeshPath ? previousMeshPath->c_str() : "");
#else
    if (previousMeshPath) setenv("REMIX_DEBUG_BAKE_MODEL_CHART_MESH", previousMeshPath->c_str(), 1);
    else unsetenv("REMIX_DEBUG_BAKE_MODEL_CHART_MESH");
#endif

  }

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

catch (const std::exception &error) { std::fprintf(stderr, "Model receiver test failed: %s\n", error.what()); return 1; }
