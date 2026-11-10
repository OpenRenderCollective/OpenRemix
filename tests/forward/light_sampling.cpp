#include "forward/bake/light_sampling.h"
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

using namespace remix;
namespace bake = forward::bake;
namespace {
  void require(bool value, const char *message) {
    if (!value)
      throw std::runtime_error(message);
  }
  uint32_t rank(const scene::LightNode &node) {
    return node.links[node.links[3] == 1 ? 0 : 2];
  }
}
int main() try {
  require(bake::lightSelectionTree({}).empty(), "No lights require no hierarchy");
  std::vector<forward::bake::TransportLight> lights(129);
  for (uint32_t i = 0; i < lights.size(); ++i) {
    lights[i].positionRadius = {0, 0, 4, .25f};
    lights[i].radiance = {float(i + 1), 1, 1, 0};
  }
  // Type sorting must not reorder captured IDs used by mixed shadows.
  lights[3].radiance[3] = 1;
  lights[3].corners = {{{-1, -1, 4, 0}, {1, -1, 4, 0}, {0, 1, 4, 0}}};
  lights[95] = lights[3];
  lights[95].radiance[3] = 2;
  lights[42].radiance = {}; // black lights still have a uniform proposal
  const auto nodes = bake::lightSelectionTree(lights);
  require(nodes.size() == 2 * lights.size() - 1 && nodes.front().links[3] == lights.size(),
          "Static hierarchy has a bounded binary node count");
  std::set<uint32_t> indices, ranks;
  const float p[3] = {}, n[3] = {0, 0, 1};
  double total = 0, minimum = 1;
  auto visit = [&](auto &&self, uint32_t index, double pdf) -> void {
    const auto &node = nodes.at(index);
    if (node.links[3] == 1) {
      require(node.links[2] < lights.size(), "Every leaf retains a captured light index");
      indices.insert(node.links[2]);
      ranks.insert(node.links[0]);
      const double mixture = .125 / lights.size() + .875 * pdf;
      total += mixture;
      minimum = std::min(minimum, mixture);
      return;
    }
    const auto &left = nodes.at(node.links[0]), &right = nodes.at(node.links[1]);
    require(rank(left) == rank(node) && rank(right) == rank(left) + left.links[3] &&
                left.links[3] + right.links[3] == node.links[3],
            "Uniform rank traversal covers both children without gaps or overlaps");
    const double a = scene::LightBvh::weight(left, p, n), b = scene::LightBvh::weight(right, p, n);
    const double q = a + b > 0 ? a / (a + b) : double(left.links[3]) / node.links[3];
    self(self, node.links[0], pdf * q);
    self(self, node.links[1], pdf * (1 - q));
  };
  visit(visit, 0, 1);
  require(indices.size() == lights.size() && ranks.size() == lights.size() && *ranks.begin() == 0 &&
              *ranks.rbegin() == lights.size() - 1,
          "Mixed sphere/triangle/black emitters preserve every source index and rank");
  require(std::abs(total - 1) < 1e-12 && minimum >= .125 / lights.size() - 1e-15,
          "The complete proposal normalizes and bounds inverse selection weights");
  const auto single = bake::lightSelectionTree(std::span(lights).first(1));
  require(single.size() == 1 && single[0].links[2] == 0, "One light keeps its original index");
  auto styled = lights[0];
  styled.shaping[3] = 97;
  const auto unstyledProposal = bake::lightSelectionTree({&styled, 1});
  const auto componentProposal = bake::lightSelectionTree({&styled, 1}, true);
  require(unstyledProposal[0].minimumPower[3] == 0 && componentProposal[0].minimumPower[3] > 0,
          "Component proposals retain styled power without changing the default shared-light proposal");
  std::printf("Bake light hierarchy: %zu emitters, %zu nodes; normalized mixed PDF, minimum %.8f\n",
              lights.size(), nodes.size(), minimum);
} catch (const std::exception &error) {
  std::fprintf(stderr, "Bake light selection test failed: %s\n", error.what());
  return 1;
}
