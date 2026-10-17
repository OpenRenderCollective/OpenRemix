#include "forward/bake/light_sampling.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace remix::forward::bake {
  std::vector<scene::LightNode> lightSelectionTree(std::span<const LightData> lights) {
    if (lights.empty())
      return {};
    if (lights.size() > std::numeric_limits<uint32_t>::max() / 2)
      throw std::length_error("Too many bake lights for selection hierarchy");
    std::vector<scene::SphereLight> sources;
    sources.reserve(lights.size());
    for (uint32_t i = 0; i < lights.size(); ++i) {
      const auto &input = lights[i];
      scene::SphereLight light;
      light.id = uint64_t(i) + 1;
      std::copy_n(input.positionRadius.begin(), 3, light.position);
      light.radius = input.positionRadius[3];
      std::copy_n(input.radiance.begin(), 3, light.radiance);
      light.triangle = uint32_t(input.radiance[3]);
      std::copy_n(input.directionCone.begin(), 3, light.direction);
      light.coneCos = input.directionCone[3];
      light.softness = input.shaping[0];
      light.focus = input.shaping[1];
      for (uint32_t vertex = 0; vertex < 3; ++vertex)
        std::copy_n(input.corners[vertex].begin(), 3, light.corners[vertex]);
      sources.push_back(light);
    }
    scene::LightBvh hierarchy;
    hierarchy.update(std::move(sources));
    auto nodes = hierarchy.nodes();
    for (auto &node : nodes)
      if (node.links[3] == 1)
        node.links[2] = uint32_t(hierarchy.lights().at(node.links[2]).id - 1);
    return nodes;
  }
} // namespace remix::forward::bake
