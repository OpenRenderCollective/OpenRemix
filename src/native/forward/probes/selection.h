#pragma once
#include "forward/probes/regions.h"
#include <span>

namespace remix::forward::probes {
  struct FittedRegion {
    Bounds bounds;
    RegionSite site;
  };
  // Spread a bounded fitting budget across validated origins, weighting distance
  // by capped clearance. Input order resolves ties within an enclosure class.
  std::vector<size_t> spreadSites(std::span<const RegionSite> sites, uint32_t budget, float clearanceCap,
                                  std::stop_token cancelled = {});
  // Greedy marginal box-volume coverage, with sheltered regions before open
  // space. Whole-region occupancy and hardware origin validation happen first.
  std::vector<size_t> selectRegions(std::span<const FittedRegion> regions, uint32_t count,
                                    std::stop_token cancelled = {});
} // namespace remix::forward::probes
