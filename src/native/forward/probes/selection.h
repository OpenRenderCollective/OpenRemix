#pragma once
#include "forward/probes/regions.h"
#include <span>

namespace remix::forward::probes {
  struct ProjectionFaceAudit {
    // Directions are grouped by the first proxy face they cross: +X,-X,+Y,-Y,+Z,-Z.
    float solidAngleFraction = 0, supportFraction = 0, nearFraction = 0, farFraction = 0;
    float missFraction = 0, backfaceFraction = 0, meanRelativeResidual = 0;
  };
  struct FittedRegion {
    Bounds bounds;
    RegionSite site;
    float quality = 1, missFraction = 0, backfaceFraction = 0;
    bool geometryEvaluated = false;
    std::array<ProjectionFaceAudit, 6> faces{};
  };
  // Spread a bounded fitting budget across validated origins, weighting distance
  // by capped clearance. Input order resolves ties within an enclosure class.
  std::vector<size_t> spreadSites(std::span<const RegionSite> sites, uint32_t budget, float clearanceCap,
                                  std::stop_token cancelled = {});
  // Geometry-weighted marginal box-volume coverage, with sheltered regions
  // before open space. Occupancy and hardware origin validation happen first.
  std::vector<size_t> selectRegions(std::span<const FittedRegion> regions, uint32_t count,
                                    std::stop_token cancelled = {});
} // namespace remix::forward::probes
