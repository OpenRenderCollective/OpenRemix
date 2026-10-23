#include "forward/bake/receiver_report.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace remix;
using namespace remix::forward;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
}
int main() {
  scene::BakeScene source;
  auto surface = std::make_shared<scene::Surface>();
  surface->vertices.resize(4);
  surface->vertices[1].position[0] = 2;
  surface->vertices[2].position[1] = 2;
  surface->vertices[3].position[0] = surface->vertices[3].position[1] = 2;
  for (auto &vertex : surface->vertices) vertex.normal[2] = 1;
  surface->indices = {0, 1, 2, 1, 3, 2};
  scene::Draw draw;
  draw.model = scene::identity();
  draw.surface = surface;
  source.draws.push_back(draw);
  source.charts.push_back({0, 0, 3, 3, {{0, 0}, {2, 0}, {0, 2}, {2, 0}, {2, 2}, {0, 2}}});
  lightmap::Layout layout;
  layout.pageSize = 4;
  layout.pages = 1;
  layout.charts = {{0, 0, 0, 3, 3}};
  std::vector<uint16_t> pixels(4 * 4 * lightmap::atlasLayers * 4);
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      const size_t address = (y * 4 + x) * 4;
      pixels[address] = 0x4000;     // 2
      pixels[address + 1] = 0x3800; // .5
      pixels[address + 2] = 0x3c00; // 1
    }
  bake::Grid grid;
  grid.header.originSpacing = {-100, -100, -100, 200};
  grid.header.dimensions = {2, 2, 2, 8};
  grid.header.settings = {0, 1, 0, 0};
  grid.probes.resize(8);
  for (auto &probe : grid.probes) {
    probe.statistics[3] = 1;
    probe.irradiance[0] = {2, .5, 1, 0};
    for (auto &moment : probe.moments) moment = {10000, 100000000, 0, 1};
  }
  const std::array<Vec3, 3> identityBasis{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
  const auto projection = bake::projectReceivers(source, layout, identityBasis);
  require(projection.triangles == 2 && projection.bounds.valid &&
              projection.bounds.minimum == Vec3{0, 0, 0} && projection.bounds.maximum == Vec3{2, 2, 0},
          "Receiver projection includes complete charted triangle extents rather than centroids");
  auto shifted = source;
  shifted.sceneScale = .5f;
  shifted.draws[0].model[12] = 10;
  const auto shiftedProjection = bake::projectReceivers(shifted, layout, identityBasis);
  require(shiftedProjection.bounds.minimum == Vec3{5, 0, 0} &&
              shiftedProjection.bounds.maximum == Vec3{6, 1, 0},
          "Projected receiver bounds apply world transforms and metre conversion once");
  auto invalidBasis = identityBasis;
  invalidBasis[1] = invalidBasis[0];
  bool invalidProjection = false;
  try { bake::projectReceivers(source, layout, invalidBasis); }
  catch (const std::invalid_argument &) { invalidProjection = true; }
  require(invalidProjection, "Projection rejects nonorthogonal bases before measuring coverage");
  auto rotatedBasis = identityBasis;
  rotatedBasis[0] = {0, 1, 0};
  rotatedBasis[1] = {-1, 0, 0};
  const auto rotatedProjection = bake::projectReceivers(source, layout, rotatedBasis);
  require(rotatedProjection.bounds.minimum == Vec3{0, -2, 0} &&
              rotatedProjection.bounds.maximum == Vec3{2, 0, 0},
          "Receiver coverage follows the supplied sun-space axes");
  std::stop_source projectionStop;
  projectionStop.request_stop();
  bool projectionCancelled = false;
  try { bake::projectReceivers(source, layout, identityBasis, projectionStop.get_token()); }
  catch (const std::runtime_error &) { projectionCancelled = true; }
  require(projectionCancelled, "Superseded receiver coverage does not keep traversing geometry");
  auto uncharted = source;
  uncharted.charts.clear();
  auto noCharts = layout;
  noCharts.charts.clear();
  const auto emptyProjection = bake::projectReceivers(uncharted, noCharts, identityBasis);
  require(!emptyProjection.bounds.valid && emptyProjection.triangles == 0,
          "Uncharted geometry does not inflate measured receiver coverage");
  auto compared = bake::compareReceivers(source, layout, pixels, grid);
  require(compared.size() == 2, "Each charted triangle produces one shared receiver");
  for (const auto &receiver : compared) {
    require(receiver.covered && receiver.chart == 0, "Probe coverage and chart provenance are retained");
    for (uint32_t c = 0; c < 3; ++c)
      require(std::abs(receiver.surface[c] - receiver.probe[c]) < 1e-6f,
              "Constant surface and probe irradiance agree without an exposure conversion");
  }
  for (uint32_t texel = 0; texel < 16; ++texel)
    pixels[(2 * 16 + texel) * 4] = 0xb800; // signed L1 z coefficient: -.5
  for (auto &probe : grid.probes) probe.irradiance[2][0] = -.5f;
  compared = bake::compareReceivers(source, layout, pixels, grid);
  require(std::abs(compared[0].surface[0] - (2 * .2820947918f - .5f * .4886025119f)) < 1e-6f &&
          std::abs(compared[0].surface[0] - compared[0].probe[0]) < 1e-6f,
          "Signed directional half coefficients retain their physical SH contribution");
  for (uint32_t texel = 0; texel < 16; ++texel) pixels[(2 * 16 + texel) * 4] = 0;
  for (auto &probe : grid.probes) probe.irradiance[2][0] = 0;
  // A linear coefficient field gives known centroid interpolation, independent
  // of the probe result. The unused atlas border must not enter chart sampling.
  constexpr uint16_t integers[] = {0, 0x3c00, 0x4000, 0x4200, 0x4400, 0x4500, 0x4600};
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x)
      pixels[(y * 4 + x) * 4] = x < 3 && y < 3 ? integers[x + y * 2] : 0x7bff;
  compared = bake::compareReceivers(source, layout, pixels, grid);
  require(std::abs(compared[0].surface[0] - 2 * .2820947918f) < 2e-6f &&
          std::abs(compared[1].surface[0] - 4 * .2820947918f) < 2e-6f,
          "Atlas comparison uses bilinear texel-centre coordinates");
  layout.mappings.resize(1);
  layout.mappings[0].width = layout.mappings[0].height = 3;
  layout.mappings[0].texcoords.assign(6, {2, 2});
  compared = bake::compareReceivers(source, layout, pixels, grid);
  require(std::abs(compared[0].surface[0] - 6 * .2820947918f) < 1e-6f,
          "Cached render mappings and chart clamps exclude the bright neighbouring atlas texels");
  source.draws[0].model[0] = 2;
  source.draws[0].model[5] = 3;
  source.draws[0].model[12] = 10;
  source.draws[0].model[13] = 20;
  for (auto &vertex : surface->vertices) {
    vertex.normal[0] = .7071067812f;
    vertex.normal[2] = .7071067812f;
  }
  compared = bake::compareReceivers(source, layout, pixels, grid, 1);
  require(compared.size() == 1 && std::abs(compared[0].position[0] - (10 + 8.f / 3)) < 2e-6f &&
          std::abs(compared[0].position[1] - 24) < 2e-6f,
          "Bounded sampling retains deterministic world-space triangle centres");
  require(std::abs(compared[0].normal[0] - .4472135955f) < 1e-6f &&
          std::abs(compared[0].normal[2] - .894427191f) < 1e-6f,
          "Nonuniform model scale uses the inverse-transpose receiver normal");
  auto partition = std::make_shared<scene::VisibilityPartition>();
  partition->closed = true;
  partition->clusterCount = 1;
  partition->nodes.push_back({{1, 0, 0, 12}, {-1, -2}, {0, 0}});
  partition->leafClusters = {0, -1};
  source.partition = partition;
  compared = bake::compareReceivers(source, layout, pixels, grid);
  require(compared[0].cluster == -1 && compared[1].cluster == 0 && compared[0].draw == 0,
          "BSP provenance separates solid/outside receivers without changing irradiance sampling");
  for (auto &probe : grid.probes) probe.statistics[3] = 0;
  require(!bake::compareReceivers(source, layout, pixels, grid, 1)[0].covered,
          "Missing probe support is reported rather than compared with invented black lighting");
  bool rejected = false;
  try { (void)bake::compareReceivers(source, layout, std::span(pixels).first(1), grid); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Truncated atlas input is rejected before sampling");
  std::stop_source cancelled;
  cancelled.request_stop();
  rejected = false;
  try { (void)bake::compareReceivers(source, layout, pixels, grid, 1, cancelled.get_token()); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "Superseded reports cannot continue collecting receivers");
  std::puts("Receiver comparison: shared positions, normals, atlas filtering, coverage and cancellation passed");
}
