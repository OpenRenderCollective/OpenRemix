#pragma once
#include "forward/probes/composition.h"

namespace remix::forward::probes {
  struct CoverageVolume {
    Bounds influence;
    Vec3 position{};
    ProjectionSpace worldToLocal = worldSpace;
    LayerCopy source;
    bool protectedCapture = false;
  };
  struct CoverageBaselineSet {
    std::vector<CoverageVolume> volumes;
    bool composed = false, disabled = false;
  };
  // Metadata only: no radiance copies. Source indices identify image layers,
  // not canonical cache entries. Composed output order follows its copy plan.
  CoverageBaselineSet automaticCoverageBaseline(std::span<const Capture> captures);
  CoverageBaselineSet composedCoverageBaseline(const Composition &composition);
}
