#pragma once
#include "forward/bindings.h"
#include "scene/bake_scene.h"

namespace remix::forward::bake {
  using UInt4 = std::array<uint32_t, 4>;
  // Transport has no runtime selection/fade weights. Keep its shader contract
  // independent so changes to per-frame light packing do not invalidate bakes.
  struct TransportLight {
    Vec4 positionRadius{}, radiance{}, directionCone{}, shaping{};
    std::array<Vec4, 3> corners{};
  };
  static_assert(sizeof(TransportLight) == 112);
  // Only ordinary RGB source-over blending describes thin surface coverage.
  // Modulating decals have a separate surface-colour model below.
  inline bool sourceOverCoverage(const scene::Blend &blend) {
    return blend.enabled && blend.src == 6 && blend.dst == 7 && blend.op == 0 && blend.mask == 15;
  }
  inline bool modulatingDecal(const scene::Draw &draw) {
    const auto &blend = draw.blend;
    return draw.decal && !draw.material->transmission && !draw.material->translucent && blend.enabled &&
           blend.src == 4 && blend.dst == 2 && blend.op == 0 && blend.mask == 15;
  }
  constexpr uint32_t suppressEmission = 1, sourceOverCoverageFlag = 2, dielectricBoundaryFlag = 4,
                     modulatingDecalFlag = 8, thinDielectricFlag = 16, reflectionOnlyFlag = 32;
  constexpr float decalReachMetres = .005f;
  struct Vertex {
    Vec4 position, normal, uv, color;
  };
  struct Material {
    DrawData shading; // dielectric remap[0/1].w store transmission-tint UV scroll rates
    // RGB image layers; fourth word: float bits of sheet thickness / measurement distance.
    UInt4 textures{}, flags{};
    Vec4 optics{}; // RGB extinction (body) or normal optical depth (sheet), refractive index
  };
  struct Texture {
    std::shared_ptr<scene::Material> sampler;
    std::shared_ptr<scene::Texture> image;
  };
  struct Inputs {
    std::vector<Vertex> vertices;
    std::vector<UInt4> triangles; // vertex indices, then material index
    std::vector<Material> materials;
    std::vector<Texture> textures; // layer zero is implicit white
    std::vector<TransportLight> lights;
    uint32_t domeLayer = 0;
  };
  Inputs pack(const scene::BakeScene &scene, double seconds = 0);
  struct Point {
    Vec4 position{}, normal{};
  }; // normal optionally supplies a surface bias direction
  struct VisibilityPoint {
    Point surface;
    uint32_t light = 0; // index in the captured BakeScene::lights
  };
  struct VisibilityTask {
    uint32_t point, light, channel; // channel is caller metadata; transport ignores it
  };
  static_assert(sizeof(VisibilityTask) == 12);
  struct Result {
    // Real SH order 0, y, z, x, xy, yz, (3z²-1), xz, (x²-y²), convolved for
    // cosine irradiance.
    std::array<Vec4, 9> irradiance{}; // grid alpha: 0..2 trace offsets, 7 sun visibility availability, 8 visibility
    std::array<Vec4, 6> moments{}; // +/-X, +/-Y, +/-Z: mean d, mean d², hit fraction, ray count
    Vec4 statistics{};             // back-face fraction, miss fraction, sample count, reserved
  };
  // GPU readback for local/sun visibility: scalar mask and nine unit direct
  // SH coefficients. Three vec4s on the wire; no unused RGB/distance moments.
  struct VisibilityResult {
    float visibility = 0;
    std::array<float, 9> irradiance{};
    std::array<float, 2> padding{};
  };
  static_assert(sizeof(VisibilityResult) == 48);
  constexpr uint32_t lightingComponentFlag = 8, containingMediaFlag = 64, lightingComponentShift = 8;
  struct Frame {
    Matrix dome;
    Vec4 sunDirection{}, sunRadiance{}, domeRadiance{}, trace{}; // domeRadiance.w: live tint animation seconds
    UInt4 counts{}, control{};
  };
  static_assert(sizeof(Vertex) == 64 && sizeof(Material) == 320);
  static_assert(sizeof(Point) == 32 && sizeof(Result) == 256 && sizeof(Frame) == 160);
} // namespace remix::forward::bake
