#include "forward/bake/policy.h"
#include <stdexcept>

namespace remix::forward::bake {
  DirectPolicy directPolicy(const scene::BakeScene &source, std::span<const uint32_t> bakedLights) {
    DirectPolicy policy;
    policy.lights.reserve(source.lights.size());
    for (const auto &light : source.lights)
      policy.lights.push_back(light.dynamic ? DirectMode::Dynamic : DirectMode::Mixed);
    for (const auto index : bakedLights) {
      if (index >= policy.lights.size() || policy.lights[index] != DirectMode::Mixed)
        throw std::invalid_argument("Invalid, repeated or moving baked diffuse light");
      policy.lights[index] = DirectMode::Baked;
    }
    return policy;
  }
  bool applyDirectPolicy(const scene::BakeScene &source, const DirectPolicy &policy,
                         std::span<LightData> lights) {
    if (policy.lights.empty())
      return false;
    if (policy.lights.size() != source.lights.size() || lights.size() != source.lights.size())
      throw std::invalid_argument("Direct lighting policy does not match captured lights");
    bool aggregate = false;
    for (size_t i = 0; i < policy.lights.size(); ++i) {
      const auto mode = policy.lights[i];
      if ((mode != DirectMode::Mixed && mode != DirectMode::Baked && mode != DirectMode::Dynamic) ||
          (source.lights[i].dynamic != (mode == DirectMode::Dynamic)))
        throw std::invalid_argument("Direct lighting policy changes moving-light membership");
      aggregate |= mode == DirectMode::Baked;
    }
    for (size_t i = 0; i < policy.lights.size(); ++i)
      lights[i].shaping[2] = policy.lights[i] == DirectMode::Baked ? 1.f : 0.f;
    return aggregate;
  }
} // namespace remix::forward::bake
