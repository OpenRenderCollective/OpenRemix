#pragma once
#include "forward/bake/config.h"
#include "forward/bake/data.h"
#include "forward/bake/probe_depth.h"
#include <optional>
#include <bit>
#include <stdexcept>
namespace remix::forward::lightmap {
  struct Layout;
}
namespace remix::forward::bake {
  // A particular lattice can miss narrow open regions without invalidating
  // the partition. Placement policies may retry a different verified lattice.
  class EmptyProbePlacement final : public std::invalid_argument {
  public:
    EmptyProbePlacement() : std::invalid_argument("Probe lattice contains no usable visibility leaves") {}
  };
  struct GridHeader {
    Vec4 originSpacing{};
    // XYZ virtual root lattice dimensions; W physical records, including refinement.
    UInt4 dimensions{};
    // CPU/cache: bias, variance, addressing mode (0 dense, 2 indexed, 4 adaptive;
    // add 1 for relocated origins), zero. Published GPU settings.z contains
    // uint32 bits: low3 addressing mode, remaining bits directory vec4 count.
    // Published GPU settings.w
    // contains uint32 bits (not a numeric float): bit0 styled-SH presence, bit31 directional-depth
    // presence; bits1..30 hold the sparse local-table offset in vec4 probe-array-relative records.
    Vec4 settings{};
  };
  static_assert(sizeof(GridHeader) == 48);
  inline float publicationSettings(size_t localTableRecords, bool styled, bool depth = false) {
    if (localTableRecords >= (uint64_t(1) << 30))
      throw std::length_error("Local probe table offset exceeds uint32 header encoding");
    return std::bit_cast<float>(uint32_t(localTableRecords * 2) | uint32_t(styled) | (depth ? 0x80000000u : 0u));
  }
  struct Grid {
    GridHeader header;
    std::vector<Point> points;
    std::vector<uint32_t> tasks;
    std::vector<Result> probes;
    ProbeDepth depth;
    // Optional virtual lattice node -> compact record + 1; zero has no probe.
    // Adaptive addressing appends four words (magic, default factor, total
    // directory words, coarse record count), then one patch reference per root
    // cell. Four-word-aligned patch blocks contain (factor+1)^3 record IDs.
    std::vector<uint32_t> lookup;
    struct Origin {
      Vec3 position{};
      float spacing = 0;
      // Aligned nodes may retain a trace from an earlier, coarser level.
      float relocationSpacing = 0;
      bool operator==(const Origin &) const = default;
    };
    // CPU-only canonical trace origins and finest spacing referencing each record.
    std::vector<Origin> origins;
  };
  inline bool indexed(const GridHeader &header) { return header.settings[2] >= 2; }
  inline bool adaptive(const GridHeader &header) { return header.settings[2] >= 4; }
  constexpr uint32_t maximumGridNodes = 1u << 20;
  constexpr uint32_t maximumRefinedCells = 1024;
  constexpr uint32_t maximumDirectoryWords = maximumGridNodes * 2 + maximumRefinedCells * 732 + 8;
  constexpr uint32_t adaptiveDirectoryMagic = 0x41475244;
  inline GridHeader publicationHeader(GridHeader header, size_t directoryWords) {
    const float mode = header.settings[2];
    if (!(mode >= 0 && mode <= 5) || float(uint32_t(mode)) != mode || directoryWords > maximumDirectoryWords ||
        ((mode < 2) != (directoryWords == 0)))
      throw std::invalid_argument("Invalid irradiance publication header");
    header.settings[2] = std::bit_cast<float>((uint32_t((directoryWords + 3) / 4) << 3) | uint32_t(mode));
    return header;
  }
  // Patch reference: bits0..1 local log2 subdivision (zero uses the default),
  // bits2..25 word offset, bits26..31 reciprocal shared-face flags.
  static_assert(maximumDirectoryWords < (1u << 26));
  inline uint32_t patchOffset(uint32_t reference) { return reference & 0x03fffffcu; }
  inline uint32_t patchFaces(uint32_t reference) { return reference >> 26; }
  inline uint32_t patchFactor(uint32_t reference, uint32_t fallback) {
    return (reference & 3u) ? 1u << (reference & 3u) : fallback;
  }
  inline uint32_t patchReference(uint32_t offset, uint32_t factor, uint32_t faces = 0) {
    if (!offset || offset >= (1u << 26) || (offset & 3u) || faces >= 64 ||
        (factor != 2 && factor != 4 && factor != 8))
      throw std::invalid_argument("Invalid irradiance patch reference");
    return offset | std::countr_zero(factor) | (faces << 26);
  }
  uint32_t gridNodes(const GridHeader &header);
  std::vector<Grid::Origin> directoryOrigins(const Grid &grid);
  // Same-resolution faces can skip the coarse fade only when every shared
  // node references the identical physical probe. Bits are -X,+X,-Y,+Y,-Z,+Z.
  std::vector<uint8_t> sharedPatchFaces(const Grid &grid);
  struct ProbeStencil {
    std::array<Vec3, 2> receiverPositions{}; // exact biased coarse/fine query endpoints for diagnostics
    std::array<uint32_t, 16> indices{};
    std::array<float, 16> weights{};
    uint32_t count = 0;
  };
  ProbeStencil probeStencil(const Grid &grid, Vec3 position, Vec3 normal);
  inline size_t lookupBytes(const Grid &grid) { return (grid.lookup.size() * sizeof(uint32_t) + 15) & ~size_t(15); }
  inline size_t gridBytes(const Grid &grid) {
    return sizeof(GridHeader) + grid.probes.size() * sizeof(Result) + lookupBytes(grid) + grid.depth.bytes();
  }
  Grid place(const scene::BakeScene &scene, const Config &config);
  Grid placeSparse(const scene::BakeScene &scene, const Config &config, const lightmap::Layout &layout,
                   std::stop_token cancelled = {});
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
