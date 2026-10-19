#pragma once
#include "forward/bake/config.h"
#include "forward/bake/data.h"
#include <optional>
#include <bit>
#include <stdexcept>
namespace remix::forward::lightmap {
  struct Layout;
}
namespace remix::forward::bake {
  struct GridHeader {
    Vec4 originSpacing{};
    UInt4 dimensions{};
    // CPU/cache: bias, variance, relocation flag, zero. Published GPU settings.w
    // contains uint32 bits (not a numeric float): bit0 styled-SH presence, remaining bits sparse local
    // table offset in vec4 records relative to the probe-array start.
    Vec4 settings{};
  };
  static_assert(sizeof(GridHeader) == 48);
  inline float publicationSettings(size_t localTableRecords, bool styled) {
    if (localTableRecords >= (uint64_t(1) << 31))
      throw std::length_error("Local probe table offset exceeds uint32 header encoding");
    return std::bit_cast<float>(uint32_t(localTableRecords * 2) | uint32_t(styled));
  }
  struct Grid {
    GridHeader header;
    std::vector<Point> points;
    std::vector<uint32_t> tasks;
    std::vector<Result> probes;
  };
  Grid place(const scene::BakeScene &scene, const Config &config);
  // Reuse untraced solid/outside slots near chart receivers. Each axis remains
  // within .45 cell widths of its original node; the allocation never grows.
  uint32_t relocate(Grid &grid, const scene::BakeScene &scene, const lightmap::Layout &layout,
                     std::stop_token cancelled = {});
  // Grid-only alpha slots 0..2 retain the displacement of the actual trace
  // origin. Surface/capture Result alpha channels keep their own contracts.
  void storeProbe(Grid &grid, uint32_t index, const Result &value);
  // Probe-only alpha: coefficients 0..2 are relocation offsets; 7 marks
  // available static sunlight visibility and 8 stores its unit visible fraction.
  // These channels never add energy to RGB irradiance.
  // CPU reference for the forward shader's visibility-aware reconstruction.
  std::optional<Vec3> sample(const Grid &grid, Vec3 position, Vec3 normal);
  std::optional<float> sampleSunVisibility(const Grid &grid, Vec3 position, Vec3 normal);
} // namespace remix::forward::bake
