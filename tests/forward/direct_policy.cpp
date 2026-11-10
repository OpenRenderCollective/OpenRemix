#include "forward/bake/policy.h"
#include <cstdio>
#include <stdexcept>
#include <remix/remix_native_ext.h>

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
  std::vector<forward::bake::TransportLight> packed(4);
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
  auto sharedIdentity = source;
  sharedIdentity.lights[1].id = sharedIdentity.lights[0].id;
  require(invalid([&] { bake::directPolicy(sharedIdentity, selected); }),
          "One source identity cannot ambiguously own both aggregated and per-light diffuse");
  const uint32_t shared[] = {0, 1};
  require(bake::directPolicy(sharedIdentity, shared).lights[1] == bake::DirectMode::Baked,
          "Several captured emitters can share an identity when their ownership policy agrees");
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
  const auto corrected = bake::directPolicy(source, selected, bake::DirectMode::BakedMixed);
  const auto fullyStatic = bake::staticAggregatePolicy(source, bake::DirectMode::Baked);
  require(fullyStatic.lights[0] == bake::DirectMode::Baked &&
              fullyStatic.lights[1] == bake::DirectMode::Baked &&
              fullyStatic.lights[2] == bake::DirectMode::Dynamic &&
              !bake::retainsVisibility(fullyStatic.lights[0]),
          "Fully baked static policy omits mixed data without changing moving-light membership");
  require(bake::applyDirectPolicy(source, corrected, packed) && packed[0].shaping[2] == 1 &&
              packed[3].shaping[2] == 1 &&
              bake::retainsVisibility(corrected.lights[0]) &&
              !bake::retainsVisibility(policy.lights[0]) &&
              !bake::retainsVisibility(corrected.lights[2]),
          "Aggregate ownership and attributed visibility are independent of moving membership");
  require(invalid([&] { bake::directPolicy(source, selected, bake::DirectMode::Mixed); }) &&
              invalid([&] { bake::directPolicy(source, selected, bake::DirectMode::Dynamic); }),
          "A selected aggregate cannot silently become an ordinary mixed or moving light");
  auto styled = source;
  auto geometry = source;
  geometry.lights[0].triangle = true;
  const auto geometryPolicy = bake::staticAggregatePolicy(geometry, bake::DirectMode::BakedMixed, true);
  require(geometryPolicy.lights[0] == bake::DirectMode::Baked &&
              geometryPolicy.lights[1] == bake::DirectMode::BakedMixed &&
              geometryPolicy.lights[2] == bake::DirectMode::Dynamic,
          "Geometry-only selection retains explicit mixed highlights without a map-wide light cap");
  geometry.lights[0].style = 32;
  geometry.lights[1].id = geometry.lights[0].id;
  const auto styledGeometry = bake::staticAggregatePolicy(geometry, bake::DirectMode::BakedMixed, true);
  require(styledGeometry.lights[0] == bake::DirectMode::Mixed &&
              styledGeometry.lights[1] == bake::DirectMode::Mixed,
          "Styled geometry and shared identities retain live controllable contributions");
  auto hinted = source;
  hinted.lights[0].bakeHint = REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT;
  hinted.lights[1].bakeHint = REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT;
  const auto hintedPolicy = bake::staticAggregatePolicy(hinted, bake::DirectMode::BakedMixed, true);
  require(!hinted.lights[0].triangle && hintedPolicy.lights[0] == bake::DirectMode::Baked &&
              hintedPolicy.lights[1] == bake::DirectMode::BakedMixed,
          "Generic policy hints classify sphere emission without inferring host-specific identities");
  require(bake::staticAggregatePolicy(hinted, bake::DirectMode::Baked).lights[1] ==
              bake::DirectMode::BakedMixed,
          "Required mixed lighting survives a broad fully baked preference");
  styled.lights[0].style = 32;
  require(invalid([&] { bake::directPolicy(styled, selected, bake::DirectMode::BakedMixed); }),
          "Retaining visibility does not make styled energy safe for permanent aggregate RGB");
  require(bake::staticAggregatePolicy(source).lights ==
              std::vector{bake::DirectMode::BakedMixed, bake::DirectMode::BakedMixed,
                          bake::DirectMode::Dynamic, bake::DirectMode::BakedMixed},
          "Automatic static policy aggregates fixed emitters while retaining moving membership");
  styled.lights[1].id = styled.lights[0].id;
  const auto styledPolicy = bake::staticAggregatePolicy(styled);
  require(styledPolicy.lights[0] == bake::DirectMode::Mixed &&
              styledPolicy.lights[1] == bake::DirectMode::Mixed &&
              styledPolicy.lights[3] == bake::DirectMode::BakedMixed,
          "One styled emitter excludes every sibling with the same identity from permanent RGB");
  source.lights[0].id = source.lights[2].id;
  const auto movingPolicy = bake::staticAggregatePolicy(source);
  require(movingPolicy.lights[0] == bake::DirectMode::Mixed &&
              movingPolicy.lights[2] == bake::DirectMode::Dynamic,
          "A shared moving identity cannot acquire partially immutable diffuse ownership");
  std::puts("Direct lighting policy: static/mixed/dynamic identities and atomic validation passed");
} catch (const std::exception &error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
