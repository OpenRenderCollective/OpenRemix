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
  {
    const std::array<Vec3, 3> triangle{Vec3{.1f, .1f, 0}, Vec3{2.8f, .1f, 0}, Vec3{.1f, 2.8f, 0}};
    bake::ReceiverTiles exact({1, 0});
    exact.include(triangle);
    require(exact.coverage().occupied == 6 && exact.coverage().rectangle == 9,
            "Triangle tile overlap excludes empty corners of its bounding rectangle");
    exact.include(triangle);
    require(exact.coverage().occupied == 6, "Overlapping receiver triangles share physical tiles");
    auto reversed = triangle;
    std::swap(reversed[0], reversed[1]);
    exact.include(reversed);
    require(exact.coverage().occupied == 6, "Receiver tile coverage is independent of triangle winding");
    auto negative = triangle;
    for (auto &vertex : negative) { vertex[0] = -vertex[0]; vertex[1] = -vertex[1]; }
    bake::ReceiverTiles mirrored({1, 0});
    mirrored.include(negative);
    require(mirrored.coverage().occupied == 6 && mirrored.coverage().rectangle == 9,
            "Negative projected coordinates use the same conservative grid convention");
    std::stop_source stopTiles;
    stopTiles.request_stop();
    bake::ReceiverTiles cancelledTiles({1, 0});
    bool stopped = false;
    try { cancelledTiles.include(triangle, stopTiles.get_token()); }
    catch (const std::runtime_error &) { stopped = true; }
    require(stopped && !cancelledTiles.coverage().occupied, "Cancelled tiling stops before populating tiles");
    bake::ReceiverTiles expanded({1, 1});
    expanded.include(triangle);
    require(expanded.coverage().occupied > 6 && expanded.coverage().occupied <= expanded.coverage().rectangle,
            "Dynamic support margin expands conservative coverage without duplicate tiles");
    bake::ReceiverTiles point({1, 0});
    point.include({Vec3{0,0,0}, Vec3{0,0,0}, Vec3{0,0,0}});
    require(point.coverage().occupied == 4, "A degenerate point on tile boundaries retains every touching tile");
    bake::ReceiverTiles bounded({1, 0, 2});
    bool exhausted = false;
    try { bounded.include(triangle); } catch (const std::length_error &) { exhausted = true; }
    require(exhausted, "Tile storage budget exhaustion is explicit rather than dropping coverage");
    bake::ReceiverTiles workBounded({1, 0, 100, 2});
    exhausted = false;
    try { workBounded.include(triangle); } catch (const std::length_error &) { exhausted = true; }
    require(exhausted && !workBounded.coverage().occupied,
            "Oversized candidate rectangles are refused before traversal or allocation");
  }

  {
    const std::array<std::array<int32_t, 2>, 4> tiles{{{1, 0}, {-1, 0}, {1, 0}, {0, 1}}};
    bake::SunDepthLayoutOptions options;
    options.pageSize = 8; options.tileTexels = 2; options.border = 1;
    options.maximumPages = 1; options.maximumBytes = 288;
    const auto layout = bake::planSunDepthLayout(tiles, options);
    require(layout && layout->tiles.size() == 3 && layout->pages == 1 && layout->depthBytes == 256 &&
                layout->directoryBytes == 32 && layout->minimum == std::array<int32_t,2>{-1,0} &&
                layout->dimensions == std::array<uint32_t,2>{3,2},
            "Sun atlas planning accounts for deduplication, borders, image pages and aligned directory storage");
    require(layout->directory == std::vector<uint32_t>{1,0,3,0,2,0},
            "Sun directory preserves negative virtual coordinates and explicit uncovered holes");
    const auto a = layout->allocation(0), b = layout->allocation(1), c = layout->allocation(2);
    require(a.page == 0 && a.x == 1 && a.y == 1 && b.x == 5 && b.y == 1 && c.x == 1 && c.y == 5 &&
                a.width == 2 && a.height == 2,
            "Physical tile allocation reserves filter borders without crossing page edges");
    const std::array<Vec3,3> basis{Vec3{1,0,0}, Vec3{0,1,0}, Vec3{0,0,-1}};
    const auto header = bake::sunDepthHeader(*layout, basis, 4, -10, 20, .002f);
    require(header.grid == std::array<int32_t,4>{-1,0,3,2} &&
                header.atlas == std::array<uint32_t,4>{8,2,1,2} && header.counts[0] == 1 &&
                header.counts[1] == 3 && header.axisZ[2] == -1 && header.sampling[0] == 4,
            "GPU lookup metadata preserves signed coordinates, atlas layout and scene-unit projection");
    auto corrupt = *layout;
    corrupt.tilesPerAxis = 0;
    bool rejectedHeader = false;
    try { bake::sunDepthHeader(corrupt, basis, 4, -10, 20, 0); }
    catch (const std::invalid_argument &) { rejectedHeader = true; }
    require(rejectedHeader, "Malformed atlas metadata cannot reach GPU division or addressing");
    --options.maximumBytes;
    require(!bake::planSunDepthLayout(tiles, options), "One byte below the complete atlas budget declines all coverage");
    options.maximumBytes = 1024;
    const std::array<std::array<int32_t,2>,5> more{{{0,0},{1,0},{2,0},{3,0},{4,0}}};
    require(!bake::planSunDepthLayout(more, options), "Page-layer limits decline rather than publish a truncated atlas");
    options.maximumPages = 2;
    const auto twoPages = bake::planSunDepthLayout(more, options);
    require(twoPages && twoPages->pages == 2 && twoPages->allocation(4).page == 1,
            "Full page occupancy advances to the next array page deterministically");
    options.maximumDirectoryWords = 1;
    require(!bake::planSunDepthLayout(tiles, options), "Sparse coordinate extents cannot exceed the bounded lookup directory");
    const auto empty = bake::planSunDepthLayout({}, options);
    require(empty && !empty->pages && !empty->depthBytes && empty->directory.empty(),
            "Empty coverage allocates neither an image nor directory");
  }
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
