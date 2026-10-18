#include "forward/bake/policy.h"
#include <stdexcept>
#include <unordered_map>
#include <remix/remix_native_ext.h>

namespace remix::forward::bake {
  DirectPolicy directPolicy(const scene::BakeScene &source, std::span<const uint32_t> bakedLights,
                            DirectMode selectedMode) {
    if (!aggregatesDiffuse(selectedMode))
      throw std::invalid_argument("Selected static lights require aggregate diffuse ownership");
    DirectPolicy policy;
    policy.lights.reserve(source.lights.size());
    for (const auto &light : source.lights)
      policy.lights.push_back(light.dynamic ? DirectMode::Dynamic : DirectMode::Mixed);
    for (const auto index : bakedLights) {
      if (index >= policy.lights.size() || policy.lights[index] != DirectMode::Mixed)
        throw std::invalid_argument("Invalid, repeated or moving baked diffuse light");
      policy.lights[index] = selectedMode;
    }
    validateDirectPolicy(source, policy);
    return policy;
  }
  bool validateDirectPolicy(const scene::BakeScene &source, const DirectPolicy &policy) {
    if (policy.lights.empty())
      return false;
    if (policy.lights.size() != source.lights.size())
      throw std::invalid_argument("Direct lighting policy does not match captured lights");
    bool aggregate = false;
    std::unordered_map<uint64_t, bool> bakedIdentities;
    for (size_t i = 0; i < policy.lights.size(); ++i) {
      const auto mode = policy.lights[i];
      if ((mode != DirectMode::Mixed && mode != DirectMode::Baked && mode != DirectMode::Dynamic &&
           mode != DirectMode::BakedMixed) ||
          (source.lights[i].dynamic != (mode == DirectMode::Dynamic)))
        throw std::invalid_argument("Direct lighting policy changes moving-light membership");
      const bool baked = aggregatesDiffuse(mode);
      aggregate |= baked;
      if (baked && source.lights[i].style)
        throw std::invalid_argument("Styled light cannot join shared aggregate irradiance");
      const auto [identity, inserted] = bakedIdentities.emplace(source.lights[i].id, baked);
      if (!inserted && identity->second != baked)
        throw std::invalid_argument("A captured light identity cannot mix aggregate and per-light ownership");
    }
    return aggregate;
  }
  DirectPolicy staticAggregatePolicy(const scene::BakeScene &source, DirectMode mode,
                                     bool bakeGeometryLights) {
    std::unordered_map<uint64_t, bool> eligible;
    for (const auto &light : source.lights) {
      const bool fixed = !light.dynamic && !light.style;
      const auto [entry, inserted] = eligible.emplace(light.id, fixed);
      if (!inserted)
        entry->second = entry->second && fixed;
    }
    std::vector<uint32_t> selected;
    for (uint32_t i = 0; i < source.lights.size(); ++i)
      if (eligible.at(source.lights[i].id))
        selected.push_back(i);
    auto policy = directPolicy(source, selected, mode);
    // Geometry emission is captured with the baked environment. Explicit
    // point/spot lights keep analytic highlights and moving-caster corrections.
    // Apply only after shared-identity eligibility excludes styled/moving sets.
    for (const auto index : selected) {
      const auto &light = source.lights[index];
      if (light.bakeHint == REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT)
        policy.lights[index] = DirectMode::BakedMixed;
      else if (bakeGeometryLights &&
               (light.triangle || light.bakeHint == REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT))
        policy.lights[index] = DirectMode::Baked;
    }
    return policy;
  }
  bool applyDirectPolicy(const scene::BakeScene &source, const DirectPolicy &policy,
                         std::span<LightData> lights) {
    const bool aggregate = validateDirectPolicy(source, policy);
    if (policy.lights.empty())
      return false;
    if (lights.size() != source.lights.size())
      throw std::invalid_argument("Direct lighting policy does not match packed lights");
    for (size_t i = 0; i < policy.lights.size(); ++i)
      lights[i].shaping[2] = aggregatesDiffuse(policy.lights[i]) ? 1.f : 0.f;
    return aggregate;
  }
} // namespace remix::forward::bake
