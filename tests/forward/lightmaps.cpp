#include "forward/lightmap/data.h"
#include "forward/lightmap/layout.h"
#include "forward/lightmap/packing.h"
#include "scene/fixtures.h"
#include <cmath>
#include <stdexcept>

using namespace remix;
using namespace remix::test;
namespace lm = remix::forward::lightmap;
namespace {
  template <typename F> void refused(F &&call) {
    bool caught = false;
    try {
      call();
    } catch (const std::exception &) {
      caught = true;
    }
    require(caught, "Invalid lightmap layout must be refused");
  }
} // namespace
int main() {
  NativeResources resources;
  const auto mat = material(resources, 1, 1, 1, 1);
  const auto mesh = quad(resources, 2, mat);
  auto placed = instance(mesh);
  placed.transform.matrix[0][0] = 2;
  placed.transform.matrix[1][1] = 3;
  placed.transform.matrix[0][3] = 10;
  const auto handle = resources.createRetained(placed);
  scene::BakeScene scene;
  scene.ready = true;
  scene.draws = resources.retained(handle)->records;
  scene.charts.push_back({0, 0, 5, 5, {{0, 0}, {4, 4}, {4, 0}, {0, 0}, {0, 4}, {4, 4}}});
  scene::freezeBakeScene(scene);
  const lm::Options options{16, 2, 2, 512};
  const lm::Options largePage{4096, 1, 0, 16777216};
  const auto packed4k = lm::packCharts(lm::prepareCharts(scene, largePage, {}), largePage, {});
  require(packed4k.pageSize == 4096 && packed4k.pages == 1 && packed4k.charts.size() == 1 &&
              packed4k.owners.empty(),
          "A 4K page can be planned without allocating or sampling the complete atlas");
  auto layout = lm::plan(scene, options);
  require(layout.points.size() == 25 && layout.addresses.size() == 25 && layout.pages == 1,
          "A chart rasterizes actual surface texels and merges its shared triangle edge");
  bool center = false;
  for (uint32_t i = 0; i < layout.points.size(); ++i) {
    const auto &p = layout.points[i];
    require(std::abs(p.position[2] - .5f) < 1e-5f && p.normal[2] < -.99f,
            "Surface points retain world placement and geometric normal fallback");
    require(layout.owners[layout.addresses[i]] == i, "Every covered texel owns its actual bake point");
    center |= std::abs(p.position[0] - 10) < 1e-5f && std::abs(p.position[1]) < 1e-5f;
  }
  require(center, "Barycentric texel positions follow nonuniform instance transforms");
  const auto &a = layout.charts[0];
  for (uint32_t y = 0; y < 9; ++y)
    for (uint32_t x = 0; x < 9; ++x)
      require(layout.owners[y * 16 + x] < 25, "Chart padding borrows only valid covered surface samples");
  require(a.x == 2 && a.y == 2 && layout.owners[15 * 16 + 15] == lm::unowned,
          "Unallocated atlas space remains unowned");
  const auto mapped = lm::meshes(scene, layout);
  require(mapped.size() == 1 && mapped[0].vertices.size() == 4 && mapped[0].indices.size() == 6,
          "Lightmap geometry preserves indexed vertex reuse inside one chart");
  const auto coarsened = lm::plan(scene, {16, 2, 2, 512, 2});
  require(coarsened.points.size() == 9 && coarsened.charts[0].width == 3 && coarsened.charts[0].height == 3,
          "Budget coarsening retains both chart endpoints at a lower texel count");
  bool firstEndpoint = false, lastEndpoint = false;
  for (const auto &point : coarsened.points) {
    firstEndpoint |= std::abs(point.position[0] - 8.4f) < 1e-5f;
    lastEndpoint |= std::abs(point.position[0] - 11.6f) < 1e-5f;
  }
  require(firstEndpoint && lastEndpoint, "Coarsening never shrinks the sampled world-surface extent");
  std::vector<remix::forward::bake::Result> coefficients(layout.points.size());
  for (auto &value : coefficients)
    value.irradiance[0] = {1, -.5f, 65000, 0};
  const auto encoded = lm::pixels(layout, coefficients);
  std::vector<lm::Sun> sunlight(layout.points.size());
  for (auto &value : sunlight) {
    value.visibility = .25f;
    value.direct = {1, -.5f, .25f, 0, 0, 0, 0, 0, 2};
  }
  const auto sunEncoded = lm::pixels(layout, coefficients, {}, {}, {}, sunlight);
  const size_t pageTexels = size_t(layout.pageSize) * layout.pageSize;
  for (size_t texel = 0; texel < layout.owners.size(); ++texel) {
    if (layout.owners[texel] == lm::unowned) continue;
    const size_t page = texel / pageTexels, pixel = texel % pageTexels;
    require(sunEncoded[((page * lm::atlasLayers) * pageTexels + pixel) * 4 + 3] == 0x3c00 &&
                sunEncoded[((page * lm::atlasLayers + 1) * pageTexels + pixel) * 4 + 3] == 0xb800 &&
                sunEncoded[((page * lm::atlasLayers + 8) * pageTexels + pixel) * 4 + 3] == 0x4000 &&
                sunEncoded[((page * lm::atlasLayers + lm::sunVisibilityLayer) * pageTexels + pixel) * 4] == 0x3400,
            "Sun SH uses indirect-layer alpha and its separate soft mask follows chart-local padding");
    require(encoded[((page * lm::atlasLayers + lm::sunVisibilityLayer) * pageTexels + pixel) * 4] == 0x3c00,
            "Missing sun results default to full visibility independently of valid black indirect lighting");
  }
  const auto centre = layout.addresses[12] * 4;
  require(encoded.size() == layout.owners.size() * lm::atlasLayers * 4 && encoded[centre] == 0x3c00 &&
              encoded[centre + 1] == 0xb800 && (encoded[centre + 2] & 0x7c00) != 0x7c00,
          "Directional atlas packing preserves signed SH and finite HDR half-float coefficients");
  require(encoded[(15 * 16 + 15) * 4] == 0, "Unallocated atlas space has defined zero radiance");
  auto thin = scene;
  thin.charts[0].width = thin.charts[0].height = 2;
  thin.charts[0].texcoords = {{.1f, .1f}, {.4f, .1f}, {.1f, .4f}};
  const auto thinLayout = lm::plan(thin, options);
  require(thinLayout.points.size() == 1 && std::abs(thinLayout.points[0].position[0] - 10.533333f) < 1e-4f &&
              std::abs(thinLayout.points[0].position[1] + .8f) < 1e-4f,
          "Subtexel charts bake a real triangle centroid instead of failing or sampling outside the surface");
  for (auto owner : thinLayout.owners)
    require(owner == lm::unowned || owner == 0, "Thin chart padding retains its single valid surface donor");
  auto fractional = scene;
  for (auto &uv : fractional.charts[0].texcoords)
    for (float &coordinate : uv) coordinate = .2f + coordinate * .9f;
  const auto fractionalLayout = lm::plan(fractional, {16, 2, 1, 512, 4});
  require(fractionalLayout.points.size() == 4,
          "Coarsened fractional faces retain each covered texel instead of collapsing to one centroid");
  for (const auto &point : fractionalLayout.points)
    require(point.position[0] >= 8.4f && point.position[0] <= 11.6f &&
                point.position[1] >= -2.4f && point.position[1] <= 2.4f,
            "Conservative coverage never moves a donor outside the actual world surface");
  auto sliver = thin;
  sliver.charts[0].width = sliver.charts[0].height = 3;
  sliver.charts[0].texcoords = {{.2f, .2f}, {1.8f, .2f}, {.2f, .4f}};
  const auto sliverLayout = lm::plan(sliver, options);
  require(sliverLayout.points.size() == 3,
          "A fractional sliver crossing three texel footprints has three surface donors despite missing all centres");
  for (const auto &point : sliverLayout.points)
    require(point.position[0] <= 11.60001f && point.position[1] >= -2.40001f &&
                2 * point.position[1] <= 3 * (point.position[0] - 10) + .0001f,
            "Clipped footprint centroids remain inside the source triangle rather than an extrapolated plane");
  scene.charts.push_back(scene.charts[0]);
  scene.charts[0].texcoords.resize(3);
  scene.charts[1].firstIndex = 3;
  scene.charts[1].texcoords.erase(scene.charts[1].texcoords.begin(), scene.charts[1].texcoords.begin() + 3);
  layout = lm::plan(scene, options);
  require(layout.pages == 2 && layout.charts[0].page != layout.charts[1].page,
          "Separate triangle charts pack with independent padding and page ownership");
  for (uint32_t y = 0; y < 9; ++y)
    for (uint32_t x = 0; x < 9; ++x) {
      const auto first = layout.owners[y * 16 + x];
      const auto second = layout.owners[256 + y * 16 + x];
      require(first != lm::unowned && second != lm::unowned && first != second,
              "Dilation cannot borrow lighting across chart boundaries");
    }
  const auto seams = lm::meshes(scene, layout);
  require(seams.size() == 1 && seams[0].vertices.size() == 6 && seams[0].indices.size() == 6,
          "Shared mesh vertices split at chart seams while triangle ownership remains intact");
  bool page0 = false, page1 = false;
  for (const auto &vertex : seams[0].vertices) {
    page0 |= vertex.coordinate[2] == 0 && vertex.coordinate[3] == 1;
    page1 |= vertex.coordinate[2] == 1 && vertex.coordinate[3] == 2;
  }
  require(page0 && page1, "Chart vertex coordinates retain atlas array page identity");
  refused([&] { lm::plan(scene, {16, 1, 2, 512}); });
  refused([&] { lm::plan(scene, {16, 2, 2, 256}); });
  refused([&] { lm::plan(scene, {8, 2, 2, 512}); });
  refused([&] { lm::plan(scene, {15, 2, 2, 512}); });
  std::stop_source stop;
  stop.request_stop();
  refused([&] { lm::plan(scene, options, stop.get_token()); });
  // Both triangles now occupy the same UV area with incompatible world positions.
  scene.charts.resize(1);
  scene.charts[0].texcoords = {{0, 0}, {4, 4}, {4, 0}, {0, 0}, {4, 4}, {4, 0}};
  refused([&] { lm::plan(scene, options); });
  std::puts(
      "Surface texel rasterization, transformed placement, isolated padding and bounded atlas pages passed");
}
