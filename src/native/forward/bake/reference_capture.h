#pragma once
#include "forward/bake/data.h"
#include <filesystem>
#include <optional>
#include <span>

namespace remix::forward::bake {
  // Private offline diagnostic, independent of probe placement/publication.
  // Input: sample count then XYZ sites (SH), or "hemisphere", sample count and
  // XYZ/unit-normal sites (cosine-weighted radiance integration, without SH).
  // "surface", sample count and XYZ/unit-normal sites uses the surface bake
  // estimator, including aggregate analytic direct lighting and SH anchoring.
  // Hemisphere rays alone do not include analytic direct light at the receiver.
  // "sh-offsets", sample count, then XYZ/first-sample sites compares multiple
  // deterministic streams at identical locations without moving the receiver.
  // "surface-offsets" adds a first-sample index after each XYZ/unit-normal site
  // to measure surface-estimator variance with analytic direct lighting intact.
  // Prefix any input with "base" to exclude separately attributed lighting
  // components when comparing the base surface atlas or base probe grid.
  // SH output includes six distance-moment vec4s (+/-X, +/-Y, +/-Z) and
  // transport statistics so offline placement studies can reconstruct visibility.
  struct Grid;
  struct ReferenceCapture {
    static std::optional<ReferenceCapture> fromEnvironment();
    static ReferenceCapture load(const std::filesystem::path &path);
    void classify(const scene::BakeScene &source);
    void consume(uint32_t first, std::span<const Result> values);
    void reportGrid(const Grid &grid) const noexcept;
    uint32_t tasks() const { return uint32_t(points.size()) * (hemisphere ? samples : 1); }
    std::vector<Point> batch(uint32_t first, uint32_t count) const;
    std::vector<Point> points;
    std::vector<uint32_t> sampleOffsets; // empty for the original SH/hemisphere formats
    uint32_t samples = 0, next = 0, received = 0;
    uint32_t bounces = 0;
    uint64_t sceneHash = 0;
    bool combinedLighting = false;
    bool baseLightingOnly = false;
    bool hemisphere = false;
    bool surface = false;

  private:
    std::filesystem::path m_output;
    std::vector<Result> m_results;
    std::vector<bool> m_completed;
    std::vector<int32_t> m_partitionClusters; // -2 unknown, -1 solid/unplayable
    std::vector<std::array<double, 3>> m_sums;
  };
}
