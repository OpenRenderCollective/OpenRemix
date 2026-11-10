#pragma once
#include "forward/probes/coverage_baseline.h"
#include "forward/probes/receiver_fits.h"
#include "forward/probes/receiver_pairs.h"
#include <stop_token>

namespace remix::forward::probes {
  constexpr uint32_t coverageGuardAxisSamples = 5;
  constexpr uint32_t coverageGuardsPerVolume = 1 + coverageGuardAxisSamples * coverageGuardAxisSamples * coverageGuardAxisSamples;
  // Stable volume order: capture position, then x/y/z cell centres. These
  // preservation points never earn surface-area gain. Callers must supply
  // separate visibility evidence before crediting a new capture with them.
  std::vector<Vec3> coverageGuardPoints(std::span<const CoverageVolume> volumes,
                                      std::stop_token cancelled = {});
  // ReceiverPair::receiver indexes guards here, never the surface receiver set.
  // No ray for an omitted/degenerate pair means unknown coverage, not clear.
  std::vector<ReceiverPair> coverageGuardPairs(std::span<const Vec3> guards, const ReceiverFits &fits,
      float minimumDistance, uint32_t budget = 32768, std::stop_token cancelled = {});
}
