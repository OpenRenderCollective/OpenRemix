#pragma once
#include "forward/probes/coverage_baseline.h"
#include <stop_token>

namespace remix::forward::probes {
  constexpr uint32_t coverageGuardAxisSamples = 5;
  constexpr uint32_t coverageGuardsPerVolume = 1 + coverageGuardAxisSamples * coverageGuardAxisSamples * coverageGuardAxisSamples;
  // Stable volume order: capture position, then x/y/z cell centres. These
  // preservation points never earn surface-area gain. Callers must supply
  // separate visibility evidence before crediting a new capture with them.
  std::vector<Vec3> coverageGuardPoints(std::span<const CoverageVolume> volumes,
                                      std::stop_token cancelled = {});
}
