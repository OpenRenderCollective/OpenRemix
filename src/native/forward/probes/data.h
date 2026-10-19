#pragma once
#include "forward/bake/data.h"
#include "forward/bake/grid.h"
#include "forward/probes/space.h"
#include <optional>
#include <span>
#include <stop_token>

namespace remix::forward::probes {
  constexpr uint32_t maxProbes = 32;
  struct Record {
    // position.w: captured solid-angle agreement with the projection box.
    // Zero is the legacy/unknown value and keeps the original blend weight.
    Vec4 minimum{}, maximum{}, influenceMinimum{}, influenceMaximum{}, position{};
    ProjectionSpace worldToLocal = worldSpace; // minimum.w stores signed overlap priority
  };
  struct Header {
    // probe count, base resolution, mip count, texels per probe
    std::array<uint32_t, 4> parameters{};
    std::array<Record, maxProbes> probes{};
  };
  static_assert(sizeof(Record) == 128 && sizeof(Header) == 4112);
  struct Capture {
    Bounds bounds;
    Vec3 position{};
    uint32_t resolution = 0;
    // Six faces, +X -X +Y -Y +Z -Z, row-major linear HDR radiance.
    std::vector<Vec4> radiance;
    // Optional receiver/blend bounds, independent of the parallax proxy.
    Bounds influence{};
    ProjectionSpace worldToLocal = worldSpace;
    int32_t priority = 0; // larger values win only where their influence contains the receiver
  };
  struct Data {
    Header header;
    // Per probe, successive mip levels; six faces at every level.
    std::vector<Vec4> texels;
  };
  struct FaceUv {
    uint32_t face;
    std::array<float, 2> uv;
  };
  // Vulkan cube-face orientation. UV is in [-1,1], also outside that range
  // when remapping a filter footprint across an edge.
  Vec3 direction(uint32_t face, float u, float v);
  FaceUv project(Vec3 direction);
  std::vector<bake::Point> rays(const Capture &capture);
  // Ordered authored/automatic candidates. The controller validates these
  // against captured geometry before fitting, including static props and masks.
  std::vector<Vec3> candidateSites(const scene::BakeScene &scene, const bake::Grid &grid, uint32_t count,
                                   std::stop_token cancelled = {});
  std::vector<Capture> place(const scene::BakeScene &scene, const bake::Grid &grid, uint32_t count,
                             uint32_t resolution, std::stop_token cancelled = {}, float detailTolerance = .5f,
                             const std::vector<Vec3> *validatedSites = nullptr);
  uint32_t levelOffset(uint32_t resolution, uint32_t level);
  // Interior props are allowed; geometry substantially beyond a proxy lowers
  // its preference where another capture overlaps. Unknown distances are neutral.
  float projectionConfidence(const Capture &capture);
  Data filter(std::span<const Capture> captures, uint32_t samples = 128, std::stop_token cancelled = {});
  std::vector<std::byte> serialize(const Data &data);
  // CPU reference for the shader's box projection, cross-face bilinear
  // filtering, roughness mip interpolation and overlapping-volume blending.
  std::optional<Vec3> sample(const Data &data, Vec3 position, Vec3 reflected, float roughness);
} // namespace remix::forward::probes
