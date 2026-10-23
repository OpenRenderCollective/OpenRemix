#pragma once
#include "forward/bake/receiver_tiles.h"
#include "forward/lightmap/layout.h"
#include <optional>
#include <span>
#include <vector>
namespace remix::forward::bake {
  struct SunDepthLayoutOptions {
    uint32_t pageSize = 2048, tileTexels = 80, border = 2, maximumPages = 8;
    uint64_t maximumBytes = 128ull << 20;
    uint32_t maximumDirectoryWords = 1u << 20;
  };
  struct SunDepthLayout {
    SunDepthLayoutOptions options;
    std::array<int32_t, 2> minimum{};
    std::array<uint32_t, 2> dimensions{};
    uint32_t pages = 0, tilesPerAxis = 0;
    uint64_t depthBytes = 0, directoryBytes = 0;
    std::vector<std::array<int32_t, 2>> tiles;
    // Row-major virtual XY -> physical slot + 1; zero requests uncovered fallback.
    std::vector<uint32_t> directory;
    lightmap::Allocation allocation(uint32_t slot) const;
  };
  struct alignas(16) SunDepthHeader {
    Vec4 axisX{}, axisY{}, axisZ{}, sampling{};
    std::array<int32_t, 4> grid{};
    std::array<uint32_t, 4> atlas{}, counts{};
  };
  static_assert(sizeof(SunDepthHeader) == 112 && offsetof(SunDepthHeader, sampling) == 48 &&
                offsetof(SunDepthHeader, grid) == 64 && offsetof(SunDepthHeader, atlas) == 80 &&
                offsetof(SunDepthHeader, counts) == 96);
  // Scene-unit axes and distances. The caller supplies the complete static
  // caster depth span; receiver bounds alone are insufficient for that range.
  SunDepthHeader sunDepthHeader(const SunDepthLayout &layout, const std::array<Vec3, 3> &basis,
      float tileWidth, float nearDepth, float farDepth, float bias);
  // Deterministic sorted physical tiles. Complete fit or explicit budget decline;
  // never retain only the first part of the requested receiver coverage.
  std::optional<SunDepthLayout> planSunDepthLayout(std::span<const std::array<int32_t, 2>> tiles,
      SunDepthLayoutOptions options = {}, std::stop_token cancelled = {});
}
