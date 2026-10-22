#include "forward/probes/receivers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace p = remix::forward::probes;
namespace s = remix::scene;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  template <class F> void invalid(F call) {
    try { call(); } catch (const std::invalid_argument &) { return; }
    require(false, "Invalid receiver input accepted");
  }
  s::BakeScene fixture() {
    s::BakeScene scene;
    scene.sceneScale = .5f;
    auto surface = std::make_shared<s::Surface>();
    const float points[][3] = {{0,0,0}, {2,0,0}, {0,1,0},
                              {10,0,0}, {16,0,0}, {10,1,0}};
    for (const auto &point : points) {
      s::Vertex vertex{};
      std::copy_n(point, 3, vertex.position);
      surface->vertices.push_back(vertex);
    }
    surface->indices = {0,1,2,3,4,5};
    s::Draw draw;
    draw.surface = surface;
    draw.terrain = true;
    draw.model[12] = 7;
    scene.draws.push_back(draw);
    s::BakeSurfaceChart chart;
    chart.width = chart.height = 2;
    chart.texcoords.resize(6);
    scene.charts.push_back(chart);
    return scene;
  }
}
int main() {
  auto source = fixture();
  const auto samples = p::sampleReceivers(source, 400);
  require(samples.size() == 400, "Receiver budget changed");
  uint32_t first = 0;
  double area = 0;
  const auto repeated = p::sampleReceivers(source, 400);
  for (size_t i = 0; i < samples.size(); ++i) {
    const auto &sample = samples[i];
    first += sample.firstIndex == 0;
    area += sample.areaMetresSquared;
    require(sample.position == repeated[i].position, "Receiver sampling is not deterministic");
    require(sample.normal == std::array<float, 3>{0,0,1}, "Missing geometric normal fallback");
    const float x = sample.position[0] - 7, y = sample.position[1];
    const bool firstTriangle = sample.firstIndex == 0;
    require(y >= 0 && x >= (firstTriangle ? 0 : 10) &&
                (x - (firstTriangle ? 0 : 10)) / (firstTriangle ? 2 : 6) + y <= 1.000001f,
            "Sample is outside its transformed triangle");
  }
  require(first == 100 && std::abs(area - 1) < 1e-10,
          "Sampling must preserve physical area, not triangle count or UV allocation");
  source.charts[0].width = source.charts[0].height = 4096;
  require(p::sampleReceivers(source, 400)[10].position == samples[10].position,
          "Atlas resolution changed receiver sampling");
  source.draws[0].model[0] = 2;
  double stretched = 0;
  for (const auto &sample : p::sampleReceivers(source, 400)) stretched += sample.areaMetresSquared;
  require(std::abs(stretched - 2) < 1e-10, "World transform did not scale receiver area");
  for (auto &vertex : source.draws[0].surface->vertices) {
    vertex.normal[0] = vertex.normal[2] = 1;
  }
  const auto smooth = p::sampleReceivers(source, 16)[0].normal;
  require(std::abs(smooth[0] - 1 / std::sqrt(5.f)) < 1e-6f &&
              std::abs(smooth[2] - 2 / std::sqrt(5.f)) < 1e-6f,
          "Smooth normals did not use inverse-transpose world transform");
  source = fixture();
  for (auto &index : source.draws[0].surface->indices) ++index;
  source.draws[0].vertexOffset = -1;
  source.draws[0].surface->indices.insert(source.draws[0].surface->indices.begin(), {99,99,99});
  source.charts[0].firstIndex = source.draws[0].firstIndex = 3;
  source.draws[0].indexCount = 6;
  const auto subrange = p::sampleReceivers(source, 400);
  require(subrange[0].position == samples[0].position && subrange[0].firstIndex == 3 &&
              subrange.back().position == samples.back().position,
          "Draw subrange or signed vertex offset changed chart geometry");
  source = fixture();
  source.draws[0].surface->indices = {0,0,0,3,4,5};
  require(p::sampleReceivers(source, 16).size() == 16, "Degenerate chart consumed sample budget");
  source.draws[0].surface->indices = {0,0,0,3,3,3};
  require(p::sampleReceivers(source, 16).empty(), "Degenerate-only scene generated receivers");
  invalid([&] { p::sampleReceivers(source, 0); });
  invalid([&] { p::sampleReceivers(source, 65537); });
  source.sceneScale = std::numeric_limits<float>::quiet_NaN();
  invalid([&] { p::sampleReceivers(source, 16); });
  std::stop_source stop;
  stop.request_stop();
  try { p::sampleReceivers(fixture(), 16, stop.get_token()); }
  catch (const std::runtime_error &) { return 0; }
  require(false, "Cancelled receiver sampling continued");
}
