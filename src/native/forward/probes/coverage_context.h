#pragma once
#include "forward/probes/coverage_baseline.h"
#include "scene/bake_scene.h"

namespace remix::forward::probes {
  struct CoveragePlanContext {
    CoverageBaselineSet baseline; // Owned metadata only; no radiance or GPU resources.
    uint64_t sceneId = 0, revision = 0, contentHash = 0, chartRevision = 0, reflectionRevision = 0;
    uint32_t scaleBits = 0;
    bool ready = false, valid = false;
  };
  CoveragePlanContext coveragePlanContext(const scene::BakeScene &source, const CoverageBaselineSet &baseline);
  // Exact metadata comparison, not a hash-only acceptance gate. This checks
  // staleness, not visual quality or the validity of a modified replacement list.
  bool matchesCoverageContext(const CoveragePlanContext &context, const scene::BakeScene &source,
                              const CoverageBaselineSet &baseline);
}
