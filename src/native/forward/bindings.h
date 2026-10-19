#pragma once
#include "forward/lighting.h"
#include "forward/shadows/plan.h"
#include <cstddef>

namespace remix::forward {
  struct SkyData {
    Matrix inverseProjection, viewToDome;
    Vec4 radiance{}, target{}; // target.z: world-unit reflection receiver bias
  };
  static_assert(sizeof(SkyData) == 160);
  // std140/std430 contracts mirrored by shaders/bindings.glsl. Keep whole
  // vectors, explicit padding and size assertions instead of compiler packing.
  struct FrameData {
    Matrix camera, view, inverseProjection;
    std::array<Matrix, cascadeCount> shadows;
    Vec4 cameraPosition{}, sunDirection{}, sunRadiance{}, ambientExposure{};
    // sunDirection.w: matching surface sun bake; sunRadiance.w: sine of disc angular radius.
    Vec4 depth{}, splits{};
    std::array<uint32_t, 4> grid{}, target{};
    std::array<Matrix, shadows::maximumLights * shadows::faces> localShadows{};
    std::array<Vec4, shadows::maximumLights> localPositions{}; // xyz origin, w far plane
    Vec4 localSettings{}; // local resolution, local radial bias, sun bias (scene units), baked direct enabled
    std::array<Vec4, 32> styleScales{}; // current/authored envelope scalar, indexed by adapter-assigned group ID
    std::array<uint32_t, 4> occlusion{}; // words per chart, chart count, texture width, indexed-mask tail
    // Capacity; zero or mixed-prefix length + 1; sparse SH word; viewmodel flags (local shadows, reflections).
    std::array<uint32_t, 4> clusterInfo{};
  };
  struct LightData {
    Vec4 positionRadius{}, radiance{}, directionCone{}, shaping{};
    // xyz triangle; [0].w shadow slot + 1, [1].w unshadowed blend,
    // [2].w: LightResponse (runtime packing only; see light_response.h).
    std::array<Vec4, 3> corners{};
    // x: selected highlight fade; y: proven empty world moving-caster map.
    // Static visibility and viewmodel self casters remain independent.
    Vec4 responseWeights{1, 0, 0, 0};
  };
  struct DrawData {
    Matrix model, normal;
    Vec4 tint{}, uv{1, 1, 0, 0}, pbr{}, emission{}, factor{};
    Vec4 remap[2]{};
    std::array<uint32_t, 4> state{}, maps{};
  };
  static_assert(sizeof(FrameData) == 4336);
  static_assert(sizeof(LightData) == 128);
  static_assert(offsetof(LightData, responseWeights) == 112);
  static_assert(sizeof(DrawData) == 272);
} // namespace remix::forward
