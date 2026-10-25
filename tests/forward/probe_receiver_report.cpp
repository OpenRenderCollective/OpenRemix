#include "forward/bake/probe_receiver_report.h"
#include "scene/bake_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace b = remix::forward::bake;
namespace s = remix::scene;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  s::Draw triangle(float z) {
    auto surface = std::make_shared<s::Surface>();
    const float points[][3] = {{0, 0, z}, {2, 0, z}, {0, 2, z}};
    for (const auto &point : points) {
      s::Vertex v{};
      std::copy_n(point, 3, v.position);
      v.normal[0] = v.normal[2] = 1;
      surface->vertices.push_back(v);
    }
    surface->indices = {0, 1, 2};
    s::Draw draw;
    draw.surface = surface;
    return draw;
  }
  template<class F> void invalid(F call) {
    try { call(); } catch (const std::invalid_argument &) { return; }
    require(false, "Invalid diagnostic ray request accepted");
  }
}
int main(int argc, char **argv) {
  s::BakeScene source;
  source.draws = {triangle(5), triangle(2)};
  std::vector<b::ReceiverRay> rays{{{.5f, .5f, 0}, {0, 0, 1}}, {{3, 3, 0}, {0, 0, 1}},
                                   {{.5f, .5f, 8}, {0, 0, -1}}, {{.5f, .5f, 0}, {1, 0, 0}}};
  auto hits = b::pickProbeReceivers(source, rays);
  require(hits[0] && hits[0]->draw == 1 && hits[0]->distance == 2 && !hits[1] &&
              hits[2] && hits[2]->draw == 0 && hits[2]->distance == 3 && !hits[3],
          "Ray picks must select nearest front/back intersections and preserve misses");
  const std::array<b::ReceiverRay, 1> normalRay{{{{.5f, .5f, 0}, {0, 0, 1}}}};
  const auto clipped = b::opaqueBiasLimits(source, normalRay, 4);
  require(clipped[0] < 2 && clipped[0] > 1.99f && b::opaqueBiasLimits(source, normalRay, 1)[0] == 1 &&
              b::opaqueBiasLimits(source, normalRay, 2)[0] == 2 && b::opaqueBiasLimits(source, normalRay, 0)[0] == 0,
          "Opaque bias limits include intervening model geometry while preserving clear/end-only segments");
  invalid([&] { b::opaqueBiasLimits(source, {}, 1); });
  require(hits[0]->normal[2] < 0 && hits[2]->normal[2] > 0 && hits[0]->geometricNormal[2] == -1,
          "Receiver normals must face the query origin");

  source.draws = {triangle(0)};
  auto &draw = source.draws[0];
  draw.model[0] = 2; draw.model[5] = 3; draw.model[10] = 4;
  draw.model[12] = 100000; draw.model[14] = 10;
  draw.surface->indices = {99, 99, 99, 1, 2, 3};
  draw.firstIndex = 3; draw.indexCount = 3; draw.vertexOffset = -1;
  rays = {{{100001, 1.5f, 20}, {0, 0, -1}}};
  hits = b::pickProbeReceivers(source, rays);
  require(hits[0] && hits[0]->firstIndex == 3 && hits[0]->position == remix::forward::Vec3{100001, 1.5f, 10},
          "Transformed subranges and signed vertex offsets must pick the actual receiver");
  const float expectedX = 2 / std::sqrt(5.f), expectedZ = 1 / std::sqrt(5.f);
  require(std::abs(hits[0]->normal[0] - expectedX) < 1e-6f &&
              std::abs(hits[0]->normal[2] - expectedZ) < 1e-6f,
          "Receiver normals must use inverse-transpose transforms before normalization");
  draw.material = std::make_shared<s::Material>();
  draw.material->transmission = true;
  require(!b::pickProbeReceivers(source, rays)[0], "Transmission is outside the opaque diagnostic");
  draw.material->transmission = false; draw.material->compare = 5;
  require(!b::pickProbeReceivers(source, rays)[0], "Untested alpha coverage must not be claimed as opaque");
  draw.ignoreAlpha = true;
  require(bool(b::pickProbeReceivers(source, rays)[0]), "Explicit ignored alpha keeps opaque triangles eligible");
  draw.decal = true;
  require(!b::pickProbeReceivers(source, rays)[0], "Decals must not replace their underlying receiver");
  draw.decal = false; draw.reflectionOnly = true;
  require(!b::pickProbeReceivers(source, rays)[0], "Reflection-only geometry must not replace a camera receiver");

  invalid([&] { b::pickProbeReceivers(source, {}); });
  invalid([&] { b::pickProbeReceivers(source, std::vector<b::ReceiverRay>(17)); });
  invalid([&] { b::pickProbeReceivers(source, std::vector<b::ReceiverRay>{{{}, {0, 0, 2}}}); });
  invalid([&] { b::pickProbeReceivers(source, std::vector<b::ReceiverRay>{{{std::numeric_limits<float>::quiet_NaN(), 0, 0}, {0, 0, 1}}}); });
  std::stop_source cancelled;
  cancelled.request_stop();
  bool stopped = false;
  try { b::pickProbeReceivers(source, rays, cancelled.get_token()); }
  catch (const std::runtime_error &) { stopped = true; }
  require(stopped, "Superseded receiver scans must cancel before work");

  require(argc == 2, "Expected owned report input path");
  const std::filesystem::path inputPath(argv[1]), outputPath(std::string(argv[1]) + ".results.json");
  require(!std::filesystem::exists(inputPath) && !std::filesystem::exists(outputPath), "Report fixture already exists");
  const char *key = "REMIX_DEBUG_PROBE_RECEIVER_RAYS";
  const auto previous = std::getenv(key) ? std::optional<std::string>(std::getenv(key)) : std::nullopt;
  auto environment = [&](const char *value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value) setenv(key, value, 1); else unsetenv(key);
#endif
  };
  b::Grid grid;
  grid.header.originSpacing = {0, 0, 0, 10};
  grid.header.dimensions = {2, 2, 2, 8};
  grid.header.settings = {0, 1, 0, 0};
  grid.probes.resize(8);
  for (auto &probe : grid.probes) {
    probe.statistics[3] = 1;
    probe.irradiance[0] = {.5f / .2820947918f, .5f / .2820947918f, .5f / .2820947918f, 0};
    for (auto &moment : probe.moments) moment = {100, 10000, 0, 1};
  }
  source.draws = {triangle(5), triangle(2)};
  source.contentHash = 123;
  auto partition = std::make_shared<s::VisibilityPartition>();
  partition->closed = true; partition->clusterCount = 1; partition->leafClusters = {0, -1};
  partition->nodes.push_back({{0, 0, 1, 3}, {-2, -1}, {0, 0}});
  source.partition = partition;
  auto writeInput = [&](const char *text) { std::ofstream(inputPath) << text; };
  writeInput("1\n.5 .5 0 0 0 1\n");
  environment(nullptr);
  b::dumpProbeReceiverReport(source, grid);
  require(!std::filesystem::exists(outputPath), "Unrequested receiver report wrote output");
  environment(argv[1]);
  b::dumpProbeReceiverReport(source, grid);
  auto readOutput = [&] {
    std::ifstream input(outputPath);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  const auto output = readOutput();
  require(output.find("\"sceneHash\":\"123\"") != std::string::npos &&
              output.find("\"draw\":1") != std::string::npos && output.find("\"normalizedWeight\":") != std::string::npos,
          "Probe report must retain scene identity, picked ownership and normalized contributions");
  require(output.find("\"geometry\":{\"totalTriangles\":1,\"truncated\":false") != std::string::npos &&
              output.find("\"vertex\":0,\"position\":[0,0,2]") != std::string::npos,
          "Receiver report retains bounded hit-draw topology and transformed corners");
  require(output.find("\"opaqueConnectionBlocked\":true") != std::string::npos &&
              output.find("\"opaqueConnectionBlocked\":false") != std::string::npos &&
              output.find("\"opaqueBlocker\":{\"draw\":0") != std::string::npos &&
              output.find("\"receiverPosition\":[0.5,0.5,2]") != std::string::npos,
          "Probe connections distinguish an intervening opaque plane from the receiver endpoint");
  require(output.find("\"containedIrradiance\":[") != std::string::npos &&
              output.find("\"containedOpaqueBlockedWeight\":") != std::string::npos,
          "Closed-partition pixel diagnostics retain the alternative estimate and blocked support");
  const auto irradiance = output.find("\"irradiance\":[");
  require(irradiance != std::string::npos, "Covered receiver has no probe irradiance");
  std::istringstream values(output.substr(irradiance + 14));
  float value = 0;
  values >> value;
  require(std::abs(value - .5f) < 1e-6f, "Report reconstruction changed constant-field energy");
  const auto visible = output.find("\"opaqueVisibleIrradiance\":[");
  require(visible != std::string::npos, "Clear opaque probe support was not reported");
  std::istringstream visibleValues(output.substr(visible + std::string("\"opaqueVisibleIrradiance\":[").size()));
  visibleValues >> value;
  require(std::abs(value - .5f) < 1e-6f, "Visible-weight renormalization changed constant-field energy");
  b::dumpProbeReceiverReport(source, grid);
  require(readOutput() == output, "Existing receiver report was overwritten");
  std::filesystem::remove(outputPath);
  const auto originalGrid = grid;
  grid.header.settings[2] = 1;
  for (auto &probe : grid.probes) probe.irradiance[2][3] = 4.4f;
  auto blocker = triangle(3);
  blocker.surface->vertices[1].position[0] = 100;
  blocker.surface->vertices[2].position[1] = 100;
  source.draws.push_back(blocker);
  b::dumpProbeReceiverReport(source, grid);
  const auto blockedOutput = readOutput();
  const auto blockedKey = blockedOutput.find("\"opaqueBlockedWeight\":");
  require(blockedKey != std::string::npos, "All-blocked fixture has no weighted connection report");
  std::istringstream blockedValues(blockedOutput.substr(blockedKey + std::string("\"opaqueBlockedWeight\":").size()));
  blockedValues >> value;
  require(value > .999f && blockedOutput.find("\"irradiance\":[") != std::string::npos &&
              blockedOutput.find("\"opaqueVisibleIrradiance\":null") != std::string::npos,
          "A covered all-blocked stencil reports missing oracle support rather than fabricated black lighting");
  grid = originalGrid; source.draws.pop_back();
  std::filesystem::remove(outputPath);
  source.draws[1].surface->indices.clear();
  source.charts.clear();
  for (uint32_t i = 0; i < 300; ++i) {
    source.draws[1].surface->indices.insert(source.draws[1].surface->indices.end(), {0, 1, 2});
    source.charts.push_back({1, i * 3, 2, 2, {{0, 0}, {1, 0}, {0, 1}}});
  }
  b::dumpProbeReceiverReport(source, grid);
  const auto bounded = readOutput();
  require(bounded.find("\"geometry\":{\"totalTriangles\":300,\"truncated\":true") != std::string::npos &&
              bounded.find("\"totalCharts\":300,\"chartsTruncated\":true") != std::string::npos &&
              bounded.find("\"chart\":255,") != std::string::npos &&
              bounded.find("\"chart\":256,") == std::string::npos,
          "Geometry and chart diagnostics explicitly cap oversized hit draws");
  std::filesystem::remove(outputPath);
  writeInput("17\n");
  b::dumpProbeReceiverReport(source, grid);
  require(!std::filesystem::exists(outputPath), "Over-budget report allocated output");
  writeInput("1\n.5 .5 0 0 0 1 trailing\n");
  b::dumpProbeReceiverReport(source, grid);
  require(!std::filesystem::exists(outputPath), "Trailing report input was silently accepted");
  std::filesystem::remove(inputPath);
  environment(previous ? previous->c_str() : nullptr);
}
