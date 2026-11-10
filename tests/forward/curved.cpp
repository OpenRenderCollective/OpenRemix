#include "forward/lighting.h"
#include "forward/lightmap/layout.h"
#include "forward/lightmap/surface_metric.h"
#include "scene/fixtures.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

using namespace remix;
using namespace remix::test;
namespace lm = forward::lightmap;
namespace {
  scene::BakeScene warpedBoundaryFan() {
    // Reduced from a legacy BSP wall: inserted boundary vertices are 5.8 mm
    // off its nominal plane. Its original UVs collapse two real fan triangles.
    const lm::curved::Point positions[] = {
        {0, 7.7216, .4064}, {0, 7.7216, 0}, {.0058353, 4.8844367, 0},
        {.0058353, .00762124, 0}, {0, 0, 0}, {0, 0, .4064},
        {.0058353, .00762124, .4064}, {.0058353, 4.8844367, .4064}};
    const std::array<float, 2> authored[] = {
        {19, 0}, {19, 1}, {12.01879f, 1}, {.01875f, 1},
        {0, 1}, {0, 0}, {.01875f, 0}, {12.01879f, 0}};
    auto surface = std::make_shared<scene::Surface>();
    for (auto p : positions) {
      scene::Vertex vertex{};
      for (uint32_t c = 0; c < 3; ++c)
        vertex.position[c] = float(p[c]);
      vertex.normal[0] = -1;
      surface->vertices.push_back(vertex);
    }
    scene::BakeSurfaceChart chart{0, 0, 20, 2, {}};
    for (uint32_t i = 1; i < 7; ++i)
      for (auto index : {0u, i, i + 1}) {
        surface->indices.push_back(index);
        chart.texcoords.push_back(authored[index]);
      }
    scene::Draw draw;
    draw.surface = surface;
    draw.material = std::make_shared<scene::Material>();
    draw.model = scene::identity();
    scene::BakeScene result;
    result.ready = true;
    result.draws.push_back(draw);
    result.charts.push_back(chart);
    return result;
  }
  scene::BakeScene patch(bool wave = false, bool hole = false) {
    scene::BakeScene scene;
    scene.ready = true;
    auto surface = std::make_shared<scene::Surface>();
    constexpr uint32_t side = 9;
    for (uint32_t y = 0; y < side; ++y)
      for (uint32_t x = 0; x < side; ++x) {
        const double u = double(x) / (side - 1), v = double(y) / (side - 1);
        scene::Vertex vertex{};
        vertex.position[0] = float(wave ? 2 * u : 2 * std::sin(1.2 * u));
        vertex.position[1] = float(2 * v);
        vertex.position[2] =
            float(wave ? .35 * std::sin(2 * std::numbers::pi * u) * std::sin(2 * std::numbers::pi * v)
                       : 2 * (1 - std::cos(1.2 * u)));
        vertex.normal[2] = 1;
        surface->vertices.push_back(vertex);
      }
    for (uint32_t y = 0; y + 1 < side; ++y)
      for (uint32_t x = 0; x + 1 < side; ++x) {
        if (hole && x == 3 && y == 3)
          continue;
        const uint32_t a = y * side + x, b = a + 1, c = b + side, d = a + side;
        surface->indices.insert(surface->indices.end(), {a, b, c, a, c, d});
      }
    scene::Draw draw;
    draw.surface = surface;
    draw.material = std::make_shared<scene::Material>();
    draw.model = scene::identity();
    scene.draws.push_back(draw);
    scene::BakeSurfaceChart chart{0, 0, side, side, {}};
    for (auto index : surface->indices)
      chart.texcoords.push_back({float(index % side), float(index / side)});
    scene.charts.push_back(std::move(chart));
    return scene;
  }
  // Independently measure the triangle Jacobian from final FLOAT atlas UVs. The
  // largest singular value bounds any direction, including diagonal luxel steps.
  void bound(const scene::BakeScene &scene, const lm::Layout &layout, bool isometric) {
    const auto &draw = scene.draws[0];
    const auto &uv = layout.mappings[0].texcoords;
    double minimum = INFINITY, maximum = 0, orientation = 0;
    for (size_t i = 0; i < uv.size(); i += 3) {
      std::array<std::array<double, 3>, 3> p;
      for (uint32_t j = 0; j < 3; ++j) {
        const auto &vertex = draw.surface->vertices[draw.surface->indices[i + j]];
        const auto world =
            forward::transformPoint(draw.model, {vertex.position[0], vertex.position[1], vertex.position[2]});
        for (uint32_t c = 0; c < 3; ++c)
          p[j][c] = double(world[c]) * scene.sceneScale;
      }
      const double a = uv[i + 1][0] - uv[i][0], b = uv[i + 2][0] - uv[i][0], c = uv[i + 1][1] - uv[i][1],
                   d = uv[i + 2][1] - uv[i][1], determinant = a * d - b * c;
      if (lm::curved::length(lm::curved::cross(lm::curved::difference(p[1], p[0]),
                                              lm::curved::difference(p[2], p[0]))) <= 1e-20) {
        require(std::abs(determinant) < 1e-10, "Zero-area geometry consumes no atlas triangle area");
        continue;
      }
      require(std::abs(determinant) > 1e-10 && determinant * orientation >= 0,
              "Curved atlas triangles have consistent winding and no folds");
      orientation = determinant;
      double uu = 0, vv = 0, cross = 0;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const double e = p[1][axis] - p[0][axis], f = p[2][axis] - p[0][axis];
        const double du = (d * e - c * f) / determinant / layout.luxelMetres,
                     dv = (a * f - b * e) / determinant / layout.luxelMetres;
        uu += du * du;
        vv += dv * dv;
        cross += du * dv;
      }
      const double discriminant = std::hypot(uu - vv, 2 * cross);
      maximum = std::max(maximum, std::sqrt(.5 * (uu + vv + discriminant)));
      minimum = std::min(minimum, std::sqrt(std::max(0., .5 * (uu + vv - discriminant))));
    }
    if (maximum > 1.00002)
      std::fprintf(stderr, "Curved float-atlas stretch: %.12g (minimum %.12g)\n", maximum, minimum);
    require(maximum <= 1.00002, "Every curved triangle respects requested world-space luxel spacing");
    if (isometric)
      require(minimum > .9998, "A developable cylinder unfolds with nearly uniform intrinsic spacing");
  }
  void onSurface(const scene::BakeScene &scene, const lm::Layout &layout) {
    namespace metric = lm::curved;
    const auto &surface = *scene.draws[0].surface;
    for (const auto &point : layout.points) {
      bool found = false;
      for (size_t i = 0; i < surface.indices.size(); i += 3) {
        metric::Point p[3];
        for (uint32_t j = 0; j < 3; ++j)
          for (uint32_t c = 0; c < 3; ++c)
            p[j][c] = surface.vertices[surface.indices[i + j]].position[c];
        const auto e = metric::difference(p[1], p[0]), f = metric::difference(p[2], p[0]);
        const auto relative =
            metric::difference({point.position[0], point.position[1], point.position[2]}, p[0]);
        const auto normal = metric::cross(e, f);
        if (std::abs(metric::dot(normal, relative)) > 1e-6 * metric::length(normal))
          continue;
        const double ee = metric::dot(e, e), ff = metric::dot(f, f), ef = metric::dot(e, f),
                     re = metric::dot(relative, e), rf = metric::dot(relative, f),
                     determinant = ee * ff - ef * ef;
        const double u = (re * ff - rf * ef) / determinant, v = (rf * ee - re * ef) / determinant;
        if (u >= -1e-5 && v >= -1e-5 && u + v <= 1 + 1e-5) {
          found = true;
          break;
        }
      }
      require(found, "Curved bake samples remain on original triangles, including hole boundaries");
    }
  }
} // namespace
void curvedLightmapTests() {
  lm::curved::Surface fan;
  fan.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, .2}, {1, 1, .2}, {0, .5, .100088}};
  fan.authored = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, .5}};
  fan.corners = {0, 2, 1, 1, 2, 3, 0, 4, 2};
  const auto correctedFan = lm::curved::correctMetric(fan, fan.authored, {});
  require(correctedFan.size() == fan.positions.size(),
          "A numerically collinear BSP fan does not invalidate the surrounding chart");
  fan.positions[4][2] = .2;
  bool collapsed = false;
  try {
    lm::curved::correctMetric(fan, fan.authored, {});
  } catch (const std::invalid_argument &) {
    collapsed = true;
  }
  require(collapsed, "Collapsed UVs over meaningful geometry fail rather than silently losing surface area");
  scene::BakeScene planarFan;
  planarFan.ready = true;
  auto fanSurface = std::make_shared<scene::Surface>();
  for (auto p : {lm::curved::Point{0, 0, 0}, {0, .5, .000088}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}}) {
    scene::Vertex vertex{};
    for (uint32_t c = 0; c < 3; ++c)
      vertex.position[c] = float(p[c]);
    vertex.normal[2] = 1;
    fanSurface->vertices.push_back(vertex);
  }
  fanSurface->indices = {0, 1, 2, 0, 3, 2, 3, 4, 2};
  scene::Draw fanDraw;
  fanDraw.surface = fanSurface;
  fanDraw.material = std::make_shared<scene::Material>();
  fanDraw.model = scene::identity();
  planarFan.draws.push_back(fanDraw);
  planarFan.charts.push_back(
      {0, 0, 2, 2, {{0, 0}, {0, .5}, {0, 1}, {0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}}});
  require(lm::plan(planarFan, {32, 1, 2, 1024, 1, .1f}).mappings[0].worldGrid,
          "An initial rounding sliver cannot turn a planar BSP face into a curved chart");
  const lm::Options options{128, 8, 2, 131072, 1, .1f};
  const auto warped = warpedBoundaryFan();
  const auto warpedLayout = lm::plan(warped, options);
  require(warpedLayout.mappings[0].intrinsic,
          "A warped boundary fan gets a non-collapsed intrinsic mapping");
  bound(warped, warpedLayout, false);
  onSurface(warped, warpedLayout);
  require(warpedLayout.mappings[0].width * warpedLayout.mappings[0].height < 2048,
          "Unfolding a narrow wall does not inflate its atlas rectangle");
  std::printf("Warped wall fan: %ux%u at %.3g m luxels, all six triangles retained\n",
              warpedLayout.mappings[0].width, warpedLayout.mappings[0].height, warpedLayout.luxelMetres);
  auto reorderedFan = warpedBoundaryFan();
  auto &fanIndices = reorderedFan.draws[0].surface->indices;
  auto &fanUvs = reorderedFan.charts[0].texcoords;
  std::rotate(fanIndices.begin(), fanIndices.begin() + 6, fanIndices.end());
  std::rotate(fanUvs.begin(), fanUvs.begin() + 6, fanUvs.end());
  const auto reorderedFanLayout = lm::plan(reorderedFan, options);
  require(reorderedFanLayout.mappings[0].intrinsic &&
              reorderedFanLayout.mappings[0].width == warpedLayout.mappings[0].width &&
              reorderedFanLayout.mappings[0].height == warpedLayout.mappings[0].height,
          "Fan unfolding recovers boundary order independently of triangle submission order");
  bound(reorderedFan, reorderedFanLayout, false);
  auto collinearFan = warpedBoundaryFan();
  auto &collinearSurface = *collinearFan.draws[0].surface;
  auto midpoint = collinearSurface.vertices[0];
  for (uint32_t c = 0; c < 3; ++c)
    midpoint.position[c] = (midpoint.position[c] + collinearSurface.vertices[1].position[c]) * .5f;
  const auto inserted = uint32_t(collinearSurface.vertices.size());
  collinearSurface.vertices.push_back(midpoint);
  collinearSurface.indices.insert(collinearSurface.indices.begin(), {0, inserted, 1});
  collinearFan.charts[0].texcoords.insert(collinearFan.charts[0].texcoords.begin(),
                                         {{19, 0}, {19, .5f}, {19, 1}});
  const auto collinearLayout = lm::plan(collinearFan, options);
  require(collinearLayout.mappings[0].intrinsic &&
              collinearLayout.mappings[0].width == warpedLayout.mappings[0].width &&
              collinearLayout.mappings[0].height == warpedLayout.mappings[0].height,
          "A zero-area boundary step preserves intrinsic fan density without wasting atlas space");
  bound(collinearFan, collinearLayout, false);
  lm::curved::Surface overlapping;
  overlapping.positions = {{0, 0, 0}, {1, 0, 0}, {-1, 1, 0}, {0, -1, 0}, {1, 1, 0}};
  overlapping.corners = {0, 1, 2, 0, 2, 3, 0, 3, 4};
  require(!lm::curved::unfoldFan(overlapping, {}),
          "An unfolded fan turning more than once cannot overlap itself in the atlas");
  auto cylinder = patch();
  const auto layout = lm::plan(cylinder, options);
  require(layout.mappings[0].curvedMetric && layout.mappings[0].intrinsic && !layout.mappings[0].worldGrid,
          "A displaced four-sided disk receives an intrinsic curved chart");
  bound(cylinder, layout, true);
  onSurface(cylinder, layout);
  auto sheared = patch();
  sheared.charts[0].width = sheared.charts[0].height = 1024;
  for (auto &uv : sheared.charts[0].texcoords)
    uv = {7 * uv[0] + .25f * uv[1], 13 * uv[1]};
  const auto rescaled = lm::plan(sheared, options);
  require(layout.mappings[0].texcoords == rescaled.mappings[0].texcoords,
          "Intrinsic geometry density is independent of authored UV scale and shear");
  auto reordered = patch();
  auto &indices = reordered.draws[0].surface->indices;
  auto &coordinates = reordered.charts[0].texcoords;
  std::rotate(indices.begin(), indices.begin() + 18, indices.end());
  std::rotate(coordinates.begin(), coordinates.begin() + 18, coordinates.end());
  for (auto &uv : coordinates)
    uv = {8 - uv[1], uv[0]};
  const auto reorderedLayout = lm::plan(reordered, options);
  require(reorderedLayout.mappings[0].intrinsic,
          "Boundary detection works when indexed-corner order starts midway along an edge");
  bound(reordered, reorderedLayout, true);
  onSurface(reordered, reorderedLayout);
  auto wave = patch(true);
  const auto waveLayout = lm::plan(wave, options);
  require(waveLayout.mappings[0].intrinsic, "A curved heightfield uses the harmonic solve");
  bound(wave, waveLayout, false);
  onSurface(wave, waveLayout);
  auto holed = patch(true, true);
  const auto holeLayout = lm::plan(holed, options);
  require(holeLayout.mappings[0].curvedMetric && !holeLayout.mappings[0].intrinsic,
          "A hole preserves authored topology with world-metric correction");
  bound(holed, holeLayout, false);
  onSurface(holed, holeLayout);
  auto transformed = patch(true);
  transformed.sceneScale = .0254f;
  transformed.draws[0].model[0] = -50;
  transformed.draws[0].model[5] = 120;
  transformed.draws[0].model[10] = 80;
  bound(transformed, lm::plan(transformed, options), false);
  auto budget = options;
  budget.pageSize = 16;
  budget.maxPages = 1;
  budget.maxTexels = 256;
  const auto fitted = lm::fit(wave, budget);
  require(fitted.luxelMetres > options.luxelMetres && fitted.mappings[0].intrinsic,
          "Atlas fitting reuses the curved shape while coarsening its spacing");
  bound(wave, fitted, false);
  onSurface(wave, fitted);
  std::printf("Curved charts: cylinder %ux%u, wave %ux%u, hole %ux%u; budget fit %.3g m\n",
              layout.mappings[0].width, layout.mappings[0].height, waveLayout.mappings[0].width,
              waveLayout.mappings[0].height, holeLayout.mappings[0].width, holeLayout.mappings[0].height,
              fitted.luxelMetres);
  std::stop_source cancellation;
  cancellation.request_stop();
  bool stopped = false;
  try {
    lm::plan(wave, options, cancellation.get_token());
  } catch (const std::runtime_error &) {
    stopped = true;
  }
  require(stopped, "Curved preparation honors cancellation before solving");
}
