#include "forward/lighting.h"
#include "forward/lightmap/data.h"
#include "forward/lightmap/layout.h"
#include "scene/fixtures.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace remix;
using namespace remix::test;
namespace lm = forward::lightmap;
namespace {
  void face(scene::BakeScene &scene, float left, float bottom, float right, float top, uint32_t width,
            uint32_t height, float depth = 0, bool reverse = false) {
    auto surface = std::make_shared<scene::Surface>();
    for (auto xy : {std::array{left, bottom}, std::array{right, bottom}, std::array{right, top},
                    std::array{left, top}}) {
      scene::Vertex vertex{};
      vertex.position[0] = xy[0];
      vertex.position[1] = xy[1];
      vertex.position[2] = depth;
      vertex.normal[2] = reverse ? -1.f : 1.f;
      surface->vertices.push_back(vertex);
    }
    surface->indices =
        reverse ? std::vector<uint32_t>{0, 2, 1, 0, 3, 2} : std::vector<uint32_t>{0, 1, 2, 0, 2, 3};
    scene::Draw draw;
    draw.surface = surface;
    draw.material = std::make_shared<scene::Material>();
    draw.model = scene::identity();
    const auto index = uint32_t(scene.draws.size());
    scene.draws.push_back(draw);
    const std::array<float, 2> uv[] = {
        {0, 0}, {float(width - 1), 0}, {float(width - 1), float(height - 1)}, {0, float(height - 1)}};
    scene::BakeSurfaceChart chart{index, 0, width, height, {}};
    for (auto vertex : surface->indices)
      chart.texcoords.push_back(uv[vertex]);
    scene.charts.push_back(std::move(chart));
  }
  scene::BakeScene adjacent(bool junction = false, float gap = 0, float depth = 0, bool reverse = false) {
    scene::BakeScene result;
    result.ready = true;
    face(result, -1.2f, -1.3f, .3f, 1.1f, 5, 7);
    if (junction) {
      face(result, .3f, -1.3f, 2.6f, .2f, 13, 3);
      face(result, .3f, .2f, 2.6f, 1.1f, 3, 19);
    } else {
      face(result, .3f + gap, -1.3f, 2.6f, 1.1f, 13, 3, depth, reverse);
    }
    return result;
  }
  float decodeHalf(uint16_t value) {
    const int exponent = value >> 10 & 31, mantissa = value & 1023;
    const float result =
        exponent ? std::ldexp(float(mantissa + 1024), exponent - 25) : std::ldexp(float(mantissa), -24);
    return value & 32768 ? -result : result;
  }
  double sample(const lm::Layout &layout, const std::vector<uint16_t> &atlas, uint32_t chart,
                std::array<double, 3> position, uint32_t layer = 0, uint32_t channel = 0) {
    const auto &mapping = layout.mappings[chart];
    const auto &allocation = layout.charts[chart];
    double coordinates[2]{};
    for (uint32_t c = 0; c < 3; ++c) {
      coordinates[0] += mapping.u[c] * position[c] / layout.luxelMetres;
      coordinates[1] += mapping.v[c] * position[c] / layout.luxelMetres;
    }
    for (uint32_t c = 0; c < 2; ++c)
      coordinates[c] -= double(mapping.origin[c]);
    const auto x = int(std::floor(coordinates[0])), y = int(std::floor(coordinates[1]));
    const double fx = coordinates[0] - x, fy = coordinates[1] - y;
    double result = 0;
    const size_t pageTexels = size_t(layout.pageSize) * layout.pageSize;
    for (uint32_t dy = 0; dy < 2; ++dy)
      for (uint32_t dx = 0; dx < 2; ++dx) {
        const auto at = ((allocation.page * lm::atlasLayers + layer) * pageTexels +
                         (allocation.y + y + dy) * layout.pageSize + allocation.x + x + dx) *
                        4;
        result += decodeHalf(atlas[at + channel]) * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
      }
    return result;
  }
  void density(const scene::BakeScene &scene, const lm::Layout &layout) {
    uint32_t actual = 0;
    for (size_t i = 0; i < layout.points.size(); ++i) {
      if (!layout.sampleCenters[i])
        continue;
      const auto chart = layout.pointCharts[i];
      const auto &mapping = layout.mappings[chart];
      if (!mapping.worldGrid)
        continue;
      for (const auto &axis : {mapping.u, mapping.v}) {
        double coordinate = 0;
        for (uint32_t c = 0; c < 3; ++c)
          coordinate += axis[c] * layout.points[i].position[c] * scene.sceneScale / layout.luxelMetres;
        require(std::abs(coordinate - std::round(coordinate)) < 1e-4,
                "Planar samples stay on the same world grid through placement, density and coarsening");
      }
      ++actual;
    }
    require(actual > 10, "World grid test contains actual integer-centre surface samples");
  }
  const forward::bake::Point &donor(const lm::Layout &layout, uint32_t chart,
                                    std::array<double, 3> position) {
    const auto &mapping = layout.mappings[chart];
    const auto &a = layout.charts[chart];
    double u = 0, v = 0;
    for (uint32_t c = 0; c < 3; ++c) {
      u += mapping.u[c] * position[c] / layout.luxelMetres;
      v += mapping.v[c] * position[c] / layout.luxelMetres;
    }
    const auto x = uint32_t(std::llround(u) - mapping.origin[0] + a.x);
    const auto y = uint32_t(std::llround(v) - mapping.origin[1] + a.y);
    return layout.points[layout.owners[(a.page * layout.pageSize + y) * layout.pageSize + x]];
  }
} // namespace
void curvedLightmapTests();
void curvedJoinTests();
int main() {
  try {
    curvedLightmapTests();
    curvedJoinTests();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Curved chart exception: %s\n", error.what());
    return 1;
  }
  auto scene = adjacent(true);
  auto &rotated = scene.charts[1];
  const float oldWidth = float(rotated.width - 1);
  std::swap(rotated.width, rotated.height);
  for (auto &uv : rotated.texcoords)
    uv = {uv[1], oldWidth - uv[0]};
  lm::Options options{16, 8, 2, 8192, 1, .5f};
  const auto layout = lm::plan(scene, options);
  require(layout.luxelMetres == .5f && layout.seamSamples > 0 && layout.pages > 1,
          "A requested metre spacing creates connected border donors");
  density(scene, layout);
  std::vector<forward::bake::Result> values(layout.points.size());
  for (size_t i = 0; i < values.size(); ++i) {
    const auto &p = layout.points[i].position;
    values[i].irradiance[0][0] = 8 + p[0] * p[0] + 2 * p[1] + .3f * p[0] * p[1];
    require(p[0] >= -1.20001f && p[0] <= 2.60001f && p[1] >= -1.30001f && p[1] <= 1.10001f &&
                std::abs(p[2]) < 1e-6,
            "Every extended donor still belongs to real geometry in the connected coplanar patch");
  }
  const auto pixels = lm::pixels(layout, values);
  const std::array<forward::bake::UInt4, 3> keys = {
      {{7, 42, lm::unowned, lm::unowned}, {42, 7, lm::unowned, lm::unowned}, {42, 99, 7, lm::unowned}}};
  std::vector<forward::Vec4> visibility(layout.points.size());
  std::vector<lm::Direct> direct(layout.points.size());
  for (size_t i = 0; i < values.size(); ++i) {
    const auto &point = layout.points[i].position;
    for (uint32_t channel = 0; channel < 4; ++channel) {
      const auto light = keys[layout.pointCharts[i]][channel];
      visibility[i][channel] = light == 7 ? .5f + .05f * point[0] + .1f * point[1] : .9f;
      direct[i][0][channel] = light == 7 ? values[i].irradiance[0][0] : 50;
    }
  }
  const auto mixed = lm::pixels(layout, values, {}, visibility, direct);
  for (double y = -.9; y <= .9; y += .05) {
    const uint32_t neighbour = y < .2 ? 1 : 2;
    require(
        std::abs(sample(layout, pixels, 0, {.3, y, 0}) - sample(layout, pixels, neighbour, {.3, y, 0})) <
            1e-6,
        "Independent atlas charts interpolate identical encoded lighting at a fractional T-junction edge");
    require(
        std::abs(sample(layout, mixed, 0, {.3, y, 0}, lm::irradianceLayers, 0) -
                 sample(layout, mixed, neighbour, {.3, y, 0}, lm::irradianceLayers, neighbour)) < 1e-6 &&
            std::abs(sample(layout, mixed, 0, {.3, y, 0}, lm::directLayer, 0) -
                     sample(layout, mixed, neighbour, {.3, y, 0}, lm::directLayer, neighbour)) < 1e-6,
        "The same physical light stays continuous even when adjacent charts assign it different channels");
  }
  for (double x = .4; x <= 2.2; x += .05)
    require(std::abs(sample(layout, pixels, 1, {x, .2, 0}) - sample(layout, pixels, 2, {x, .2, 0})) < 1e-6,
            "The other edge of the T-junction also shares a continuous bilinear footprint");
  for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
    const auto &a = layout.charts[chart];
    for (uint32_t y = a.y - options.padding; y < a.y + a.height + options.padding; ++y)
      for (uint32_t x = a.x - options.padding; x < a.x + a.width + options.padding; ++x) {
        const auto owner = layout.owners[(a.page * layout.pageSize + y) * layout.pageSize + x];
        require(owner != lm::unowned && layout.pointCharts[owner] == chart,
                "Cross-face samples preserve the receiving chart's per-light channel ownership");
      }
  }
  const auto meshes = lm::meshes(scene, layout);
  require(meshes.size() == 3, "Runtime geometry uses the generated mappings on every draw");
  options.texelStep = 2;
  const auto coarse = lm::plan(scene, options);
  require(coarse.luxelMetres == 1,
          "One global coarsening factor changes spacing without stretching endpoints");
  density(scene, coarse);
  options.texelStep = 1;
  for (auto isolated : {adjacent(false, .05f), adjacent(false, 0, .005f), adjacent(false, 0, 0, true)}) {
    const auto separated = lm::plan(isolated, options);
    require(separated.seamSamples == 0,
            "Disconnected gaps, parallel planes and opposing faces cannot share lighting donors");
  }
  auto opening = adjacent(false, .05f);
  face(opening, .3f, -1.3f, .35f, -.8f, 3, 3); // joins the two sides around an open gap
  const auto aroundHole = lm::plan(opening, options);
  require(donor(aroundHole, 0, {.5, 0, 0}).position[0] <= .30001f &&
              std::abs(donor(aroundHole, 0, {.5, -1, 0}).position[0] - .5f) < 1e-5f,
          "A connected patch cannot filter across its hole, but a genuinely covered bridge can share donors");
  options.pageSize = 32;
  auto displaced = adjacent();
  displaced.draws[1].surface->vertices[2].position[2] = .1f;
  const auto curved = lm::plan(displaced, options);
  require(curved.mappings[0].worldGrid && !curved.mappings[1].worldGrid &&
              curved.mappings[1].curvedMetric && curved.mappings[1].intrinsic,
          "Nonplanar geometry receives an intrinsic world-metric chart");
  auto transformed = adjacent();
  transformed.sceneScale = .0254f;
  for (auto &draw : transformed.draws) {
    draw.model[0] = 20;
    draw.model[5] = 30;
    draw.model[12] = 200;
  }
  options.luxelMetres = .1f;
  density(transformed, lm::plan(transformed, options));
  for (auto &draw : transformed.draws)
    draw.model[0] = -20;
  const auto mirrored = lm::plan(transformed, options);
  density(transformed, mirrored);
  require(mirrored.seamSamples > 0 && mirrored.mappings[0].normalKey == layout.mappings[0].normalKey,
          "Mirrored instance transforms retain the shading-facing plane and connected world grid");
  scene::BakeScene large;
  large.ready = true;
  face(large, -80, -80, 80, 80, 5, 5);
  const lm::Options budget{32, 1, 1, 1024, 1, 1};
  const auto fitted = lm::fit(large, budget);
  require(fitted.pages == 1 && fitted.luxelMetres == 6 && fitted.charts[0].width == 29 &&
              fitted.charts[0].height == 29 && fitted.owners.size() == 1024,
          "Budget refinement chooses a verified intermediate density instead of doubling straight to eight "
          "metres");
  density(large, fitted);
  auto automatic = budget;
  automatic.fillBudget = true;
  const auto filled = lm::fit(large, automatic);
  require(filled.luxelMetres < fitted.luxelMetres && filled.charts[0].width >= 29 &&
              filled.charts[0].height >= 29 && filled.owners.size() == 1024,
          "Automatic density refines the fit between integer spacing steps within the same allocated budget");
  scene::BakeScene smallMap;
  smallMap.ready = true;
  face(smallMap, 0, 0, 1, 1, 2, 2);
  const auto smallFilled = lm::fit(smallMap, automatic);
  require(smallFilled.luxelMetres < .04f && smallFilled.charts[0].width >= 29 &&
              smallFilled.pages == 1 && smallFilled.owners.size() == 1024,
          "Small maps spend spare atlas capacity on detail rather than stopping at requested metre spacing");
  automatic.luxelMetres = .01f;
  const auto independent = lm::fit(smallMap, automatic);
  require(independent.addresses == smallFilled.addresses && independent.luxelMetres == smallFilled.luxelMetres,
          "Saved manual spacing cannot cap or perturb automatic atlas density");
  density(smallMap, smallFilled);
  automatic.pageSize = 64;
  automatic.maxTexels = 4096;
  const auto largerAtlas = lm::fit(smallMap, automatic);
  require(largerAtlas.luxelMetres < smallFilled.luxelMetres * .5f &&
              largerAtlas.points.size() > smallFilled.points.size() * 3 &&
              largerAtlas.owners.size() == 4096,
          "A larger target atlas buys finer surface detail rather than unused capacity");
  const auto directPlan = lm::plan(large, {32, 1, 1, 1024, 6, 1});
  require(fitted.addresses == directPlan.addresses && fitted.owners == directPlan.owners &&
              fitted.points.size() == directPlan.points.size(),
          "Budget probes leave the final surface samples identical to a direct plan at the selected spacing");
  for (size_t i = 0; i < fitted.points.size(); ++i)
    require(fitted.points[i].position == directPlan.points[i].position &&
                fitted.points[i].normal == directPlan.points[i].normal,
            "Density refinement preserves the actual ray origins and bias normals of the selected grid");
  scene::BakeScene tight;
  tight.ready = true;
  face(tight, 0, 0, 2, 6, 3, 7);
  face(tight, 0, 0, 2, 6, 3, 7, 2);
  const auto bordered = lm::fit(tight, {8, 1, 1, 64, 1, 1});
  const auto bounded = lm::fit(tight, {8, 1, 0, 64, 1, 1});
  require(bounded.luxelMetres == 1 && bounded.pages == 1 && bordered.luxelMetres > 1,
          "Texel-centre chart bounds preserve finer world density within the same atlas memory budget");
  const auto boundedMeshes = lm::meshes(tight, bounded);
  for (const auto &mesh : boundedMeshes)
    for (const auto &vertex : mesh.vertices) {
      const auto &a = bounded.charts[uint32_t(vertex.coordinate[3]) - 1];
      require(vertex.atlasBounds == forward::Vec4{(a.x + .5f) / 8, (a.y + .5f) / 8,
                                                   (a.x + a.width - .5f) / 8, (a.y + a.height - .5f) / 8},
              "Every mapped vertex carries its chart's exact first/last texel-centre bounds");
      require(vertex.coordinate[0] >= vertex.atlasBounds[0] && vertex.coordinate[0] <= vertex.atlasBounds[2] &&
                  vertex.coordinate[1] >= vertex.atlasBounds[1] && vertex.coordinate[1] <= vertex.atlasBounds[3],
              "Outward-rounded surface UVs stay inside the bounded subrectangle");
    }
  bool exhausted = false;
  try {
    lm::fit(large, {8, 1, 4, 64, 1, 1});
  } catch (const std::length_error &) {
    exhausted = true;
  }
  require(exhausted,
          "Impossible minimum-size packing fails after bounded coarsening without dropping a face");
  std::stop_source cancelled;
  cancelled.request_stop();
  bool stopped = false;
  try {
    lm::fit(large, budget, cancelled.get_token());
  } catch (const std::runtime_error &) {
    stopped = true;
  }
  require(stopped, "Budget refinement propagates cancellation rather than treating it as a packing failure");
  std::puts("Metre-space density, global coarsening, connected seams, T-junctions, isolation and curved "
            "surface metrics passed");
}
