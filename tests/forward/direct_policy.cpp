#include "forward/bake/policy.h"
#include <cstdio>
#include <stdexcept>

using namespace remix;
namespace bake = forward::bake;
namespace {
  void require(bool value, const char *message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <typename F> bool invalid(F &&operation) {
    try { operation(); } catch (const std::invalid_argument &) { return true; }
    return false;
  }
}
int main() try {
  scene::BakeScene source;
  source.lights.resize(4);
  source.lights[2].dynamic = true;
  for (uint32_t i = 0; i < source.lights.size(); ++i)
    source.lights[i].id = 100 - i; // membership uses captured order, not sorted IDs
  const uint32_t selected[] = {3, 0};
  const auto policy = bake::directPolicy(source, selected);
  require(policy.lights == std::vector{bake::DirectMode::Baked, bake::DirectMode::Mixed,
                                      bake::DirectMode::Dynamic, bake::DirectMode::Baked},
          "Static diffuse membership preserves mixed/dynamic identity in captured order");
  std::vector<forward::LightData> packed(4);
  for (uint32_t i = 0; i < packed.size(); ++i) {
    packed[i].positionRadius[0] = float(i);
    packed[i].radiance = {1, 2, 3, 0};
  }
  require(bake::applyDirectPolicy(source, policy, packed), "Selected lights request aggregate sampling");
  for (uint32_t i = 0; i < packed.size(); ++i)
    require(packed[i].shaping[2] == float(i == 0 || i == 3) && packed[i].positionRadius[0] == float(i) &&
                packed[i].radiance == forward::Vec4{1, 2, 3, 0},
            "Policy tags never replace, recolour or reorder the captured emitters");
  const uint32_t moving[] = {2}, duplicate[] = {0, 0}, outside[] = {4};
  require(invalid([&] { bake::directPolicy(source, moving); }) &&
              invalid([&] { bake::directPolicy(source, duplicate); }) &&
              invalid([&] { bake::directPolicy(source, outside); }),
          "Moving, duplicate and unknown static-light memberships are rejected");
  auto malformed = policy;
  malformed.lights[3] = bake::DirectMode::Dynamic;
  require(invalid([&] { bake::applyDirectPolicy(source, malformed, packed); }) && packed[0].shaping[2] == 1,
          "Invalid membership cannot partially overwrite an earlier valid policy");
  malformed.lights.pop_back();
  require(invalid([&] { bake::applyDirectPolicy(source, malformed, packed); }),
          "Policy/source length disagreement is rejected before upload");
  const auto mixed = bake::directPolicy(source, {});
  require(!bake::applyDirectPolicy(source, mixed, packed) && packed[0].shaping[2] == 0,
          "An all-mixed policy disables aggregate sampling and clears previous static tags");
  std::puts("Direct lighting policy: static/mixed/dynamic identities and atomic validation passed");
} catch (const std::exception &error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
