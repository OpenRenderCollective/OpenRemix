#include "forward/bake/sun_antialiasing.h"
#include "forward/half.h"
#include "forward/lightmap/data.h"
#include "forward/lightmap/layout.h"
#include "forward/lightmap/interior_donors.h"
#include "forward/lightmap/packing.h"
#include "forward/lightmap/receiver_footprint.h"
#include "forward/lightmap/receiver_sampler.h"
#include "scene/fixtures.h"
#include "sun_coverage.h"
#include <cmath>
#include <cstring>
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
  {
    lm::Layout covered;
    covered.pageSize = 4;
    covered.pages = 1;
    covered.charts = {{0, 0, 0, 2, 4}, {0, 2, 0, 2, 4}};
    covered.points.resize(16);
    covered.pointCharts.resize(16);
    covered.owners.resize(16);
    std::vector<forward::bake::Result> values(16);
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x) {
        const uint32_t point = x < 2 ? y * 2 + x : 8 + y * 2 + x - 2;
        covered.owners[y * 4 + x] = point;
        covered.pointCharts[point] = x < 2 ? 0 : 1;
        covered.points[point].position[0] = float(point);
        values[point].statistics = {point >= 8 || (point >= 2 && point <= 5) ? 1.f : 0.f, 0, 256, 0};
        values[point].irradiance[0][0] = float(point + 1);
      }
    // Open, ambiguous and undersampled points must remain usable donors.
    values[1].statistics = {1, .2f, 256, 0};
    values[6].statistics = {.5f, 0, 256, 0};
    values[7].statistics = {1, 0, 8, 0};
    const auto original = covered.owners;
    const auto originalPoints = covered.points;
    const auto originalCharts = covered.pointCharts;
    const auto repaired = lm::repairInteriorDonors(covered, values);
    require(repaired.hiddenSamples == 12 && repaired.repairedTexels == 4 && repaired.unresolvedCharts == 1,
            "Interior repair is conservative and reports charts without safe exterior donors");
    const uint32_t expected[] = {0, 1, 0, 1, 6, 7, 6, 7};
    for (uint32_t y = 0; y < 4; ++y)
      for (uint32_t x = 0; x < 4; ++x)
        require(covered.owners[y * 4 + x] == (x < 2 ? expected[y * 2 + x] : original[y * 4 + x]),
                "Nearest exterior lighting stays within its receiving chart");
    std::vector<forward::Vec4> visibility(16);
    std::vector<lm::Direct> direct(16);
    std::vector<lm::Sun> sun(16);
    for (uint32_t i = 0; i < 16; ++i) {
      visibility[i][0] = .1f + float(i) / 32;
      direct[i][0][0] = .3f + float(i);
      sun[i].visibility = .2f + float(i) / 32;
      sun[i].direct[0] = .4f + float(i);
    }
    const auto pixels = lm::pixels(covered, values, {}, visibility, direct, sun);
    require(forward::half::decode(pixels[4 * 4]) == 1,
            "An interior texel packs its exterior donor instead of bright hidden lighting");
    require(pixels[(lm::visibilityLayer * 16 + 4) * 4] == forward::half::encode(.1f) &&
                pixels[(lm::directLayer * 16 + 4) * 4] == forward::half::encode(.3f) &&
                pixels[(lm::sunVisibilityLayer * 16 + 4) * 4] == forward::half::encode(.2f) &&
                pixels[4 * 4 + 3] == forward::half::encode(.4f),
            "Mixed visibility, local direct and sunlight use the same repaired chart donor");
    require(covered.pointCharts == originalCharts && covered.points.size() == originalPoints.size() &&
                std::memcmp(covered.points.data(), originalPoints.data(),
                            covered.points.size() * sizeof(covered.points.front())) == 0,
            "Interior donor repair preserves world samples and chart identities");
    require(lm::repairInteriorDonors(covered, values).repairedTexels == 0,
            "Interior donor repair is idempotent");
    std::stop_source stopped;
    stopped.request_stop();
    refused([&] { lm::repairInteriorDonors(covered, values, stopped.get_token()); });
    auto invalid = covered;
    invalid.owners[0] = 8;
    refused([&] { lm::repairInteriorDonors(invalid, values); });
  }
  {
    lm::Mesh mesh;
    mesh.vertices.resize(4);
    mesh.indices = {0, 1, 2};
    for (uint32_t i = 0; i < 3; ++i) mesh.vertices[i].coordinate[3] = 1;
    require(lm::fullyMapped(mesh), "Unused unmapped vertices do not prevent a complete charted draw");
    mesh.vertices[1].coordinate[3] = 0;
    require(!lm::fullyMapped(mesh), "Partially charted draws retain probe fallback for uncovered triangles");
    mesh.indices.clear();
    require(!lm::fullyMapped(mesh), "Empty meshes do not select a chart-only lighting specialization");
  }
  {
    lm::Mesh mesh;
    mesh.vertices.resize(3);
    mesh.vertices[0].directLights = {7, 2, 7, ~0u};
    mesh.vertices[1].directLights = {2, ~0u, ~0u, ~0u};
    require(lm::directOwners(mesh) == std::vector<uint32_t>{2, 7},
            "Draw direct ownership excludes invalid channels and merges repeated chart owners");
    mesh.vertices[2].directLights[0] = 4000000;
    require(lm::directOwners(mesh) == std::vector<uint32_t>{2, 7, 4000000},
            "Unknown ownership survives metadata preparation for conservative runtime fallback");
  }
  {
    using Footprint = lm::ReceiverFootprint;
    const Footprint square({Footprint::Uv{-8, -8}, {8, -8}, {0, 8}}, {0, 0});
    require(std::abs(square.area() - 1) < 1e-6 &&
                std::abs(square.centroid()[0]) < 1e-6 && std::abs(square.centroid()[1]) < 1e-6,
            "Receiver footprint clips to one luxel rather than the triangle bounds");
    const Footprint partial({Footprint::Uv{0, -.5f}, {.5f, .5f}, {-.5f, .5f}}, {0, 0});
    require(std::abs(partial.area() - .5) < 1e-6, "Partial luxel coverage retains its true area");
    std::array<double, 2> mean{};
    uint32_t visible = 0;
    constexpr uint32_t count = 16384;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t bits = i, reversed = 0;
      for (uint32_t bit = 0; bit < 14; ++bit) {
        reversed = (reversed << 1) | (bits & 1);
        bits >>= 1;
      }
      const double u = (i + .5) / count, v = (reversed + .5) / count;
      const auto p = partial.sample(u, v), full = square.sample(u, v);
      require(p[0] >= -.500001f && p[0] <= .500001f && p[1] >= -.500001f && p[1] <= .500001f &&
                  p[1] >= 2 * std::abs(p[0]) - .500001f,
              "Every footprint sample stays inside both the luxel and the actual receiving triangle");
      require(full[0] >= -.500001f && full[0] <= .500001f && full[1] >= -.500001f && full[1] <= .500001f,
              "Area sampling cannot leave the clipped convex polygon");
      for (uint32_t axis = 0; axis < 2; ++axis) mean[axis] += p[axis] / count;
      visible += p[0] > 0;
    }
    require(std::abs(mean[0]) < .002 && std::abs(mean[1] - 1. / 6) < .002,
            "Area-weighted sample mean matches the analytic triangle centroid");
    require(std::abs(double(visible) / count - .5) < .002,
            "A hard shadow bisecting a luxel integrates to half coverage instead of a binary centre test");
    const Footprint empty({Footprint::Uv{2, 2}, {3, 2}, {2, 3}}, {0, 0});
    const Footprint line({Footprint::Uv{-.5f, 0}, {0, 0}, {.5f, 0}}, {0, 0});
    require(empty.empty() && line.empty(), "Absent and zero-area geometry never manufacture coverage");
    refused([&] { empty.sample(.5, .5); });
    refused([&] { square.sample(1, .5); });
    refused([&] { square.sample(.5, -.1); });
    // Two triangles meeting at a diagonal must account for exactly one square,
    // so future quadrature can combine donors without double-counting the seam.
    const Footprint left({Footprint::Uv{-.5f, -.5f}, {.5f, .5f}, {-.5f, .5f}}, {0, 0});
    const Footprint right({Footprint::Uv{-.5f, -.5f}, {.5f, -.5f}, {.5f, .5f}}, {0, 0});
    require(std::abs(left.area() + right.area() - 1) < 1e-6,
            "Shared triangle edges neither lose nor duplicate receiver area");
    const Footprint reversed({Footprint::Uv{-.5f, .5f}, {.5f, .5f}, {0, -.5f}}, {0, 0});
    require(std::abs(reversed.area() - partial.area()) < 1e-6 &&
                std::abs(reversed.centroid()[1] - partial.centroid()[1]) < 1e-6,
            "Receiver footprint measure is independent of triangle winding");
    uint32_t state = 7193;
    const auto random = [&] {
      state = state * 1664525u + 1013904223u;
      return double(state >> 8) / 16777216.;
    };
    for (uint32_t trial = 0; trial < 4096; ++trial) {
      std::array<Footprint::Uv, 3> triangle;
      for (auto &vertex : triangle)
        for (float &value : vertex) value = float(8 * random() - 4);
      const Footprint clipped(triangle, {0, 0});
      require(clipped.area() <= 1.00001, "Clipping never allocates more than one luxel of coverage");
      if (clipped.empty()) continue;
      const auto edge = [](Footprint::Uv a, Footprint::Uv b, Footprint::Uv p) {
        return (double(b[0]) - a[0]) * (double(p[1]) - a[1]) -
               (double(b[1]) - a[1]) * (double(p[0]) - a[0]);
      };
      const double orientation = edge(triangle[0], triangle[1], triangle[2]) > 0 ? 1 : -1;
      for (uint32_t sample = 0; sample < 16; ++sample) {
        const auto point = clipped.sample(random(), random());
        require(std::abs(point[0]) <= .50001 && std::abs(point[1]) <= .50001,
                "Random clipped footprint stays inside its luxel");
        for (uint32_t corner = 0; corner < 3; ++corner)
          require(orientation * edge(triangle[corner], triangle[(corner + 1) % 3], point) >= -1e-5,
                  "Random clipped footprint stays on its actual receiving triangle in either winding");
      }
    }
  }
  {
    // The last 8-high row is full, but both earlier 12-high rows have a 12-wide
    // gap. A last-row-only shelf allocator incorrectly needs another page.
    std::vector<lm::ChartShape> shapes(5);
    const std::array<std::array<uint32_t, 2>, 5> sizes{{{20, 12}, {20, 12}, {32, 8}, {12, 8}, {12, 8}}};
    for (size_t i = 0; i < shapes.size(); ++i) {
      shapes[i].frame.width = sizes[i][0];
      shapes[i].frame.height = sizes[i][1];
    }
    const auto packed = lm::packCharts(shapes, {32, 1, 0, 1024}, {});
    require(packed.pages == 1, "Short charts reuse retained row gaps within a bounded page");
    std::vector<bool> occupied(1024);
    for (const auto &chart : packed.charts)
      for (uint32_t y = chart.y; y < chart.y + chart.height; ++y)
        for (uint32_t x = chart.x; x < chart.x + chart.width; ++x) {
          require(x < 32 && y < 32 && !occupied[y * 32 + x],
                  "Reusing a shelf never overlaps another chart or leaves its page");
          occupied[y * 32 + x] = true;
        }
  }
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
  const auto compact = lm::fit(scene, {1024, 16, 2, 1048576});
  require(compact.pageSize == 16 && compact.pages == 1 && compact.points.size() == layout.points.size(),
          "A small chart selects a smaller page without sacrificing a single surface sample");
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
  {
    lm::ReceiverSampler sampler(scene, layout);
    for (uint32_t i = 0; i < layout.points.size(); ++i)
      if (std::abs(layout.points[i].position[0] - 10) < 1e-5 && std::abs(layout.points[i].position[1]) < 1e-5)
        sunlightCoverage(sampler, i);
    bool crossesDiagonal = false;
    for (uint32_t i = 0; i < layout.points.size(); ++i) {
      const auto sites = sampler.sample(i), repeated = sampler.sample(i);
      require(std::memcmp(sites.data(), repeated.data(), sizeof(sites)) == 0,
              "Receiver quadrature is stable across repeated batches and light identities");
      bool above = false, below = false;
      for (const auto &site : sites) {
        require(site.position[0] >= 8.39999f && site.position[0] <= 11.60001f &&
                    site.position[1] >= -2.40001f && site.position[1] <= 2.40001f &&
                    std::abs(site.position[2] - .5f) < 1e-5f && site.normal[2] < -.99f,
                "Footprint sites follow transformed real geometry and never leave chart edges");
        const float diagonal = (site.position[0] - 10) / 1.6f - site.position[1] / 2.4f;
        above |= diagonal > 1e-5f;
        below |= diagonal < -1e-5f;
      }
      crossesDiagonal |= above && below;
    }
    require(crossesDiagonal, "Shared-edge receiver footprints sample both actual triangles");
    std::vector<lm::Sun> clear(layout.points.size());
    remix::forward::bake::SunAntialiasing noEdges(scene, layout, clear);
    require(noEdges.count() == 0, "Uniform sunlight does not schedule extra receiver rays");
    for (uint32_t i = 0; i < clear.size(); ++i)
      clear[i].visibility = layout.points[i].position[0] < 10 ? 0 : 1;
    remix::forward::bake::SunAntialiasing edges(scene, layout, clear);
    require(edges.count() > 0 && edges.count() < layout.points.size(),
            "Sunlight refinement is restricted to the detected contrast band");
    remix::forward::bake::SunAntialiasing lowBudget(scene, layout, clear, {}, 4);
    require(lowBudget.siteCount() == 4 &&
                lowBudget.sites(0, lowBudget.count()).size() == lowBudget.count() * 4,
            "Low sample budgets retain four sunlight origins");
    std::array<remix::forward::bake::Point, 8> invalidSites;
    refused([&] { sampler.sample(0, invalidSites); });
    refused([&] { remix::forward::bake::SunAntialiasing invalid(scene, layout, clear, {}, 8); });
    const auto all = edges.sites(0, edges.count());
    for (uint32_t i = 0; i < edges.count(); ++i) {
      const auto one = edges.sites(i, 1);
      require(std::memcmp(one.data(), all.data() + i * edges.siteCount(), one.size() * sizeof(one[0])) == 0,
              "Adaptive sunlight sites retain identity across batch boundaries");
    }
    refused([&] { edges.sites(edges.count(), 1); });
    std::stop_source cancelled;
    cancelled.request_stop();
    refused([&] { remix::forward::bake::SunAntialiasing stopped(scene, layout, clear, cancelled.get_token()); });
    auto bent = scene;
    bent.draws[0].surface = std::make_shared<scene::Surface>(*scene.draws[0].surface);
    bent.draws[0].surface->vertices[2].position[2] += .8f;
    const auto bentLayout = lm::plan(bent, options);
    lm::ReceiverSampler bentSampler(bent, bentLayout);
    for (uint32_t i = 0; i < bentLayout.points.size(); ++i)
      for (const auto &site : bentSampler.sample(i)) {
        const float u = (site.position[0] - 8.4f) / 3.2f, v = (site.position[1] + 2.4f) / 4.8f;
        require(std::abs(site.position[2] - (.5f + .8f * std::min(u, v))) < 2e-5f,
                "Bent surface samples lie on their actual triangle, never an averaged or extrapolated plane");
      }
    auto extended = layout;
    extended.receiverSamples[0] = 0;
    extended.points[0].position[0] += .125f;
    lm::ReceiverSampler seamSampler(scene, extended);
    for (const auto &site : seamSampler.sample(0))
      require(site.position == extended.points[0].position && site.normal == extended.points[0].normal,
              "Verified seam-extension donors retain their original actual surface sites");
    extended.receiverSources.assign(extended.points.size(), lm::unowned);
    uint32_t footprintSource = lm::unowned;
    for (uint32_t i = 0; i < extended.points.size(); ++i)
      if (std::abs(extended.points[i].position[0] - 10) < 1e-5 &&
          std::abs(extended.points[i].position[1]) < 1e-5) footprintSource = i;
    require(footprintSource != lm::unowned, "Seam fixture has a complete central footprint");
    extended.receiverSources[0] = footprintSource;
    lm::ReceiverSampler sharedSampler(scene, extended);
    const auto donorSites = sharedSampler.sample(0), sourceSites = sharedSampler.sample(footprintSource);
    require(std::memcmp(donorSites.data(), sourceSites.data(), sizeof(donorSites)) == 0,
            "Coplanar seam donors retain their source receiver's antialiasing footprint");
    sunlightCoverage(sharedSampler, 0);
    std::vector<lm::Sun> seamShadow(extended.points.size());
    for (auto &value : seamShadow) value.visibility = 0;
    seamShadow[0].visibility = 1;
    auto edgeLayout = extended;
    edgeLayout.owners[edgeLayout.addresses[0]] = 0;
    edgeLayout.owners[edgeLayout.addresses[0] + 1] = footprintSource;
    remix::forward::bake::SunAntialiasing seamEdges(scene, edgeLayout, seamShadow);
    require(std::find(seamEdges.receivers().begin(), seamEdges.receivers().end(), 0) != seamEdges.receivers().end(),
            "Sunlight edge refinement admits seam donors with verified source footprints");
    extended.receiverSources[footprintSource] = 0;
    refused([&] { sharedSampler.sample(0); });

  }
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
