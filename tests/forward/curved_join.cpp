#include "forward/lightmap/data.h"
#include "forward/lightmap/surface_walk.h"
#include "scene/fixtures.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace remix;
using namespace remix::test;
namespace lm = forward::lightmap;
namespace {
  struct Strip {
    uint32_t draw, columns;
    double start;
  };
  Strip addCylinderStrip(scene::BakeScene &scene, double first, double last, uint32_t columns, uint32_t rows,
                         bool rotate, bool hole = false) {
    auto surface = std::make_shared<scene::Surface>();
    for (uint32_t y = 0; y <= rows; ++y)
      for (uint32_t x = 0; x <= columns; ++x) {
        const double angle = x == columns ? last : first + (last - first) * x / columns;
        scene::Vertex vertex{};
        vertex.position[0] = float(2 * std::sin(angle));
        vertex.position[1] = -1.3f + 2.4f * float(y) / rows;
        vertex.position[2] = float(2 * (1 - std::cos(angle)));
        vertex.normal[0] = float(-std::sin(angle));
        vertex.normal[2] = float(std::cos(angle));
        surface->vertices.push_back(vertex);
      }
    for (uint32_t y = 0; y < rows; ++y)
      for (uint32_t x = 0; x < columns; ++x) {
        if (hole && x == 1 && y + 1 < rows)
          continue;
        const uint32_t a = y * (columns + 1) + x, b = a + 1, c = b + columns + 1, d = a + columns + 1;
        surface->indices.insert(surface->indices.end(), {a, b, c, a, c, d});
      }
    scene::Draw draw;
    draw.surface = surface;
    draw.material = std::make_shared<scene::Material>();
    draw.model = scene::identity();
    const auto drawId = uint32_t(scene.draws.size());
    scene.draws.push_back(draw);
    scene::BakeSurfaceChart chart{
        drawId, 0, rotate ? rows + 1 : columns + 1, rotate ? columns + 1 : rows + 1, {}};
    for (auto index : surface->indices) {
      const auto x = index % (columns + 1), y = index / (columns + 1);
      chart.texcoords.push_back(rotate ? std::array{float(y), float(columns - x)}
                                       : std::array{float(x), float(y)});
    }
    scene.charts.push_back(std::move(chart));
    double extent = 0;
    for (uint32_t x = 0; x < columns; ++x) {
      const auto &a = surface->vertices[x].position, &b = surface->vertices[x + 1].position;
      extent += std::hypot(double(b[0]) - a[0], double(b[2]) - a[2]);
    }
    return {drawId, columns, first < 0 ? -extent : 0};
  }
  // Arc length on the actual piecewise-linear cylinder, not its analytic ideal.
  double arc(const scene::BakeScene &scene, const std::array<Strip, 2> &strips, const forward::Vec4 &point) {
    for (auto strip : strips) {
      const auto &vertices = scene.draws[strip.draw].surface->vertices;
      double along = strip.start;
      for (uint32_t x = 0; x < strip.columns; ++x) {
        const auto &a = vertices[x].position, &b = vertices[x + 1].position;
        const double dx = b[0] - a[0], dz = b[2] - a[2], length = std::hypot(dx, dz),
                     t = ((point[0] - a[0]) * dx + (point[2] - a[2]) * dz) / (length * length);
        if (t >= -1e-5 && t <= 1 + 1e-5 &&
            std::hypot(point[0] - a[0] - t * dx, point[2] - a[2] - t * dz) < 1e-5)
          return along + t * length;
        along += length;
      }
    }
    require(false, "A curved seam donor belongs to an actual cylinder segment");
    return 0;
  }
  double half(uint16_t value) {
    const int exponent = value >> 10 & 31, mantissa = value & 1023;
    const double result =
        exponent ? std::ldexp(double(mantissa + 1024), exponent - 25) : std::ldexp(double(mantissa), -24);
    return value & 32768 ? -result : result;
  }
  double sample(const scene::BakeScene &scene, const lm::Layout &layout, const std::vector<uint16_t> &pixels,
                uint32_t chart, uint32_t layer, uint32_t channel, double height) {
    namespace walk = lm::seam;
    const auto &source = scene.charts[chart];
    const auto &surface = *scene.draws[source.draw].surface;
    const auto &allocation = layout.charts[chart];
    const walk::Point target{float(2 * std::sin(.31)), height, float(2 * (1 - std::cos(.31)))};
    double u = 0, v = 0;
    bool found = false;
    for (size_t first = 0; first < source.texcoords.size(); first += 3) {
      walk::Point points[3];
      for (uint32_t i = 0; i < 3; ++i)
        for (uint32_t c = 0; c < 3; ++c)
          points[i][c] = surface.vertices[surface.indices[first + i]].position[c];
      const auto e = walk::subtract(points[1], points[0]), f = walk::subtract(points[2], points[0]),
                 relative = walk::subtract(target, points[0]);
      const auto normal = walk::cross(e, f);
      if (std::abs(walk::dot(normal, relative)) > 1e-6 * walk::length(normal))
        continue;
      const double ee = walk::dot(e, e), ff = walk::dot(f, f), ef = walk::dot(e, f),
                   re = walk::dot(relative, e), rf = walk::dot(relative, f), determinant = ee * ff - ef * ef;
      const double a = (re * ff - rf * ef) / determinant, b = (rf * ee - re * ef) / determinant;
      if (a < -1e-6 || b < -1e-6 || a + b > 1 + 1e-6)
        continue;
      const auto &uv = layout.mappings[chart].texcoords;
      u = (1 - a - b) * uv[first][0] + a * uv[first + 1][0] + b * uv[first + 2][0];
      v = (1 - a - b) * uv[first][1] + a * uv[first + 1][1] + b * uv[first + 2][1];
      found = true;
      break;
    }
    require(found, "The comparison point lies on the shared world-space boundary");
    const int x = int(std::floor(u)), y = int(std::floor(v));
    double result = 0;
    const size_t pageTexels = size_t(layout.pageSize) * layout.pageSize;
    for (uint32_t dy = 0; dy < 2; ++dy)
      for (uint32_t dx = 0; dx < 2; ++dx) {
        const auto address = ((allocation.page * lm::atlasLayers + layer) * pageTexels +
                              (allocation.y + y + dy) * layout.pageSize + allocation.x + x + dx) *
                                 4 +
                             channel;
        result += half(pixels[address]) * (dx ? u - x : 1 - u + x) * (dy ? v - y : 1 - v + y);
      }
    return result;
  }
} // namespace
void curvedJoinTests() {
  scene::BakeScene scene;
  scene.ready = true;
  const std::array strips{addCylinderStrip(scene, -.63, .31, 7, 1, false),
                          addCylinderStrip(scene, .31, .93, 11, 4, true)};
  lm::Options options{16, 8, 2, 8192, 1, .23f};
  const auto layout = lm::plan(scene, options);
  require(layout.curvedSeamSamples > 10, "A curved T-junction creates real filtering-only surface donors");
  std::vector<forward::bake::Result> irradiance(layout.points.size());
  std::vector<forward::Vec4> visibility(layout.points.size());
  std::vector<lm::Direct> direct(layout.points.size());
  for (size_t i = 0; i < layout.points.size(); ++i) {
    const auto &p = layout.points[i].position;
    const double s = arc(scene, strips, p);
    const float field = float(2 + .2 * s + .1 * p[1]);
    irradiance[i].irradiance[0][0] = field;
    const auto slot = layout.pointCharts[i] == 0 ? 0 : 2;
    visibility[i] = {.9f, .9f, .9f, .9f};
    visibility[i][slot] = float(.5 + .05 * s + .1 * p[1]);
    direct[i][0] = {50, 50, 50, 50};
    direct[i][0][slot] = field;
    if (!layout.receiverSamples[i]) {
      const auto &allocation = layout.charts[layout.pointCharts[i]];
      require(layout.addresses[i] / (layout.pageSize * layout.pageSize) == allocation.page,
              "Curved donors retain the receiving chart and its atlas page");
    }
  }
  const auto pixels = lm::pixels(layout, irradiance, {}, visibility, direct);
  // The long edge meets four short edges, with independently rotated UVs.
  double error = 0;
  for (double y = -1.2; y < .99; y += .013) {
    const double a = sample(scene, layout, pixels, 0, 0, 0, y), b = sample(scene, layout, pixels, 1, 0, 0, y);
    error = std::max(error, std::abs(a - b));
    require(std::abs(a - (2 + .1 * y)) < .003 && std::abs(b - (2 + .1 * y)) < .003,
            "Curved chart edges reconstruct the same world-space linear lighting field");
    require(std::abs(sample(scene, layout, pixels, 0, lm::visibilityLayer, 0, y) -
                     sample(scene, layout, pixels, 1, lm::visibilityLayer, 2, y)) < .002 &&
                std::abs(sample(scene, layout, pixels, 0, lm::directLayer, 0, y) -
                         sample(scene, layout, pixels, 1, lm::directLayer, 2, y)) < .003,
            "Curved seams preserve a physical light assigned to different atlas channels");
  }
  auto separated = scene;
  separated.draws[1].surface = std::make_shared<scene::Surface>(*scene.draws[1].surface);
  for (auto &vertex : separated.draws[1].surface->vertices)
    vertex.position[0] += .01f;
  require(lm::plan(separated, options).curvedSeamSamples == 0,
          "A nearby curved surface cannot bridge an open gap");
  auto opposite = scene;
  opposite.draws[1].surface = std::make_shared<scene::Surface>(*scene.draws[1].surface);
  for (auto &vertex : opposite.draws[1].surface->vertices)
    for (auto &c : vertex.normal)
      c = -c;
  require(lm::plan(opposite, options).curvedSeamSamples == 0,
          "Opposite-facing curved surfaces do not share lighting");
  double furthest = 0;
  for (size_t i = 0; i < layout.points.size(); ++i)
    if (!layout.receiverSamples[i] && layout.pointCharts[i] == 0)
      furthest = std::max(furthest, arc(scene, strips, layout.points[i].position));
  require(furthest > .15, "A coarse curved gutter walks across multiple small neighbouring triangles");
  scene::BakeScene holed;
  holed.ready = true;
  const std::array holeStrips{addCylinderStrip(holed, -.63, .31, 7, 1, false),
                              addCylinderStrip(holed, .31, .93, 11, 4, true, true)};
  const auto holeLayout = lm::plan(holed, options);
  require(holeLayout.curvedSeamSamples > 0, "The open-strip test contains a working shared curved edge");
  for (size_t i = 0; i < holeLayout.points.size(); ++i)
    if (!holeLayout.receiverSamples[i] && holeLayout.pointCharts[i] == 0 &&
        holeLayout.points[i].position[1] < .49f)
      require(arc(holed, holeStrips, holeLayout.points[i].position) < .114,
              "A straight surface walk stops at an opening even though a covered route exists through the "
              "top bridge");
  scene::BakeScene stacked;
  stacked.ready = true;
  addCylinderStrip(stacked, -.63, .31, 7, 1, false);
  addCylinderStrip(stacked, -.63, .31, 7, 1, false);
  require(lm::plan(stacked, options).curvedSeamSamples == 0,
          "Overlapping curved faces with matching borders cannot donate through each other");
  auto scaled = scene;
  scaled.sceneScale = .0254f;
  for (auto &draw : scaled.draws) {
    draw.surface = std::make_shared<scene::Surface>(*draw.surface);
    for (auto &vertex : draw.surface->vertices)
      for (auto &c : vertex.position)
        c /= scaled.sceneScale;
  }
  require(lm::plan(scaled, options).curvedSeamSamples > 0,
          "Curved adjacency and donor positions respect the host's world-unit conversion");
  std::printf("Curved seams: %u surface donors, maximum encoded linear-field difference %.6g\n",
              layout.curvedSeamSamples, error);
}
