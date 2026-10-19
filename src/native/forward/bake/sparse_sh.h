#pragma once
#include "forward/bake/styles.h"
#include "forward/lightmap/layout.h"
#include <span>
#include <functional>
#include <optional>

namespace remix::forward::bake::sparse {
  constexpr uint32_t magic = 0x48535352, version = 2, tileSide = 16;
  constexpr uint32_t headerWords = 12, entryWords = 8, coefficientWords = 9;
  using Coefficients = std::array<Vec4, 9>; // RGB only; alpha/geometry never belongs to a style
  struct Dimensions {
    uint32_t pageSize = 0, pages = 0, probes = 0;
    bool operator==(const Dimensions &) const = default;
  };
  // Maps contain record index + 1; zero denotes an entirely black block.
  // Surface records form 16x16 tiles; probes use individual records.
  struct Component {
    uint32_t style = 0;
    std::vector<uint32_t> surface, probes, coefficients;
  };
  class Builder {
  public:
    Builder(Dimensions dimensions, uint64_t maximumBytes);
    void add(uint32_t style, const lightmap::Layout &layout, std::span<const Result> surfaces,
             std::span<const Result> probes, std::stop_token cancelled = {});
    // Streaming source: at most one 16x16 encoding tile is materialized. A
    // failed/cancelled addition leaves every previously completed group intact.
    using SurfaceSample = std::function<Coefficients(uint32_t page, uint32_t x, uint32_t y)>;
    using ProbeSample = std::function<Coefficients(uint32_t probe)>;
    // Optional conservative occupancy: false guarantees the entire surface tile
    // is black. Tile indices follow page-major row order. Unknown means sample.
    using SurfaceTilePresent = std::function<bool(uint32_t tile)>;
    void addSampled(uint32_t style, const SurfaceSample &surface, const ProbeSample &probe,
                    std::stop_token cancelled = {}, const SurfaceTilePresent &tilePresent = {});
    std::vector<uint32_t> serialize(std::stop_token cancelled = {}) const;
    uint64_t bytes() const;
    const Dimensions &dimensions() const { return m_dimensions; }

  private:
    Dimensions m_dimensions;
    uint64_t m_maximumBytes;
    std::vector<Component> m_components;
  };
  // Full validation before a cache or external blob reaches the GPU. Requires
  // canonical offsets, sorted unique styles, finite half values and block bounds.
  void validate(std::span<const uint32_t> words, Dimensions expected,
                std::span<const uint32_t> styles, uint64_t maximumBytes);
  // CPU reference sampling of validated data. Returns signed contributions;
  // callers combine these with base SH before the final irradiance clamp.
  Coefficients probe(std::span<const uint32_t> words, uint32_t style, uint32_t index);
  Coefficients surface(std::span<const uint32_t> words, uint32_t style,
                       uint32_t page, uint32_t x, uint32_t y);
  // Add independently captured components with matching dimensions/style IDs.
  // Inputs are validated and remain unchanged. The result is all-or-nothing:
  // storage exhaustion returns nullopt, so the caller can retain its old table.
  std::optional<std::vector<uint32_t>> sumComponents(std::span<const uint32_t> first,
      std::span<const uint32_t> second, Dimensions dimensions, std::span<const uint32_t> allowedStyles,
      uint64_t maximumBytes, std::stop_token cancelled = {});
  Vec3 surfaceIrradiance(std::span<const uint32_t> words, std::span<const StyleScale> scales,
                         Vec4 coordinate, Vec3 normal);
} // namespace remix::forward::bake::sparse
