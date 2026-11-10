#pragma once
#include "forward/probes/data.h"
#include "forward/probes/selection.h"

namespace remix::forward::probes {
  // Stream geometry-only readback into one small assessment per fitted region.
  // No lighting samples or full-resolution cubemaps are allocated for candidates.
  // Identical origins share queries, while each fit retains independent statistics.
  class ProjectionAudit {
  public:
    static constexpr uint32_t resolution = 8, maximumOrigins = 160, maximumCandidates = 3 * maximumOrigins;
    explicit ProjectionAudit(std::vector<FittedRegion> regions, std::stop_token cancelled = {}, bool collectPlanes = false);
    std::span<const bake::Point> rays() const { return m_rays; }
    bool observe(uint32_t first, std::span<const bake::Result> results);
    std::vector<FittedRegion> finish() const;
    // Reuse completed optional diagnostic samples at identical origins and
    // directions. No new traversal; unobserved origins are refused.
    std::vector<FittedRegion> reassess(std::vector<FittedRegion> regions) const;

  private:
    struct Statistics {
      double agreement = 0, total = 0, missed = 0, back = 0;
      double nearer = 0, farther = 0, residual = 0, absoluteResidual = 0;
    };
    struct RegionStatistics {
      Statistics total;
      std::array<Statistics, 6> faces;
      std::array<Bounds, 6> hitBounds;
    };
    // Optional compact first-hit history: at most 160 * 384 samples, no RGB.
    struct GeometrySample { float distance, back, miss; };
    std::vector<GeometrySample> m_samples;
    bool m_reusable = false;
    struct PlaneSample { float coordinate, weight; };
    // Allocated only for explicitly requested plane diagnostics. At most one
    // sample per face-crossing ray and candidate (480 * 384 samples total).
    std::vector<std::array<std::vector<PlaneSample>, 6>> m_planes;
    struct Origin {
      Vec3 position;
      std::vector<size_t> regions;
    };
    std::vector<FittedRegion> m_regions;
    std::vector<bake::Point> m_rays;
    std::vector<RegionStatistics> m_statistics;
    std::vector<Origin> m_origins;
    std::vector<uint8_t> m_seen;
    uint32_t m_completed = 0;
    std::stop_token m_cancelled;
  };
} // namespace remix::forward::probes
