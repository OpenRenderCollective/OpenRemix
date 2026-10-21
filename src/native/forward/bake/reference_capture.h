#pragma once
#include "forward/bake/data.h"
#include <filesystem>
#include <optional>
#include <span>

namespace remix::forward::bake {
  // Private offline diagnostic, independent of probe placement/publication.
  // Input: sample count then XYZ sites (SH), or "hemisphere", sample count and
  // XYZ/unit-normal sites (cosine-weighted radiance integration, without SH).
  struct ReferenceCapture {
    static std::optional<ReferenceCapture> fromEnvironment();
    static ReferenceCapture load(const std::filesystem::path &path);
    void consume(uint32_t first, std::span<const Result> values);
    uint32_t tasks() const { return uint32_t(points.size()) * (hemisphere ? samples : 1); }
    std::vector<Point> batch(uint32_t first, uint32_t count) const;
    std::vector<Point> points;
    uint32_t samples = 0, next = 0, received = 0;
    uint32_t bounces = 0;
    uint64_t sceneHash = 0;
    bool combinedLighting = false;
    bool hemisphere = false;

  private:
    std::filesystem::path m_output;
    std::vector<Result> m_results;
    std::vector<bool> m_completed;
    std::vector<std::array<double, 3>> m_sums;
  };
}
