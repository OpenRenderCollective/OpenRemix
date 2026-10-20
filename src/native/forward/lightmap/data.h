#pragma once
#include "forward/lightmap/layout.h"
#include <span>
namespace remix::forward::lightmap {
  constexpr uint32_t irradianceLayers = 9, visibilityLayer = 9, directLayer = 10, sunVisibilityLayer = 19,
                     atlasLayers = 20;
  constexpr uint32_t atlasBytesPerTexel = atlasLayers * 4 * sizeof(uint16_t);
  // Nine signed, cosine-convolved coefficients of unit-radiance direct light;
  // each vector component belongs to the chart's corresponding light channel.
  using Direct = std::array<Vec4, irradianceLayers>;
  struct Sun {
    float visibility = 1;
    std::array<float, irradianceLayers> direct{};
  };
  struct Vertex {
    scene::Vertex surface;
    Vec4 coordinate{}; // normalized atlas UV, page, chart index + 1 (zero is invalid)
    bake::UInt4 lights{~0u, ~0u, ~0u, ~0u};
    bake::UInt4 directLights{~0u, ~0u, ~0u, ~0u};
    Vec4 atlasBounds{}; // normalized first/last texel centres; flat per triangle
  };
  static_assert(sizeof(Vertex) == 112);
  struct Mesh {
    uint32_t draw = 0;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
  };
  std::vector<uint32_t> directOwners(const Mesh &mesh);
  bool fullyMapped(const Mesh &mesh);
  // Duplicate only vertices whose chart coordinates disagree, preserving
  // indexed reuse within charts and explicit seams between them.
  std::vector<Mesh> meshes(const scene::BakeScene &scene, const Layout &layout,
                           std::stop_token cancelled = {}, std::span<const bake::UInt4> lightChannels = {},
                           std::span<const bake::UInt4> directChannels = {});
  // RGBA16F: 0..8 indirect RGB SH + unit sun SH in alpha; 9 local visibility;
  // 10..18 unit local direct SH; 19 sun visibility. Donors preserve chart ownership.
  std::vector<uint16_t> pixels(const Layout &layout, std::span<const bake::Result> results,
                               std::stop_token cancelled = {}, std::span<const Vec4> visibility = {},
                               std::span<const Direct> direct = {}, std::span<const Sun> sun = {});
} // namespace remix::forward::lightmap
