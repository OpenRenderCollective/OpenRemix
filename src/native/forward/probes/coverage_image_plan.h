#pragma once
#include "forward/probes/receiver_coverage_plan.h"

namespace remix::forward::probes {
  enum class CoverageImageSource { Baseline, Local };
  struct CoverageImageCopy { CoverageImageSource source; uint32_t layer; };
  struct CoverageReadyLayer { uint32_t candidate, layer; };
  struct CoverageReadyImage {
    Header header;
    CoveragePlanContext context;
    std::vector<CoverageReadyLayer> layers;
  };
  struct CoverageImagePlan {
    Header header;
    LayerIdentities identities{};
    std::array<Header, 2> sources; // Exact Baseline/Local metadata expected at recording.
    std::vector<CoverageImageCopy> copies;
    CoveragePlanContext context;
  };
  // Build a metadata/copy recipe only. The caller must retain both image
  // owners through submission fences and recheck the context at activation.
  CoverageImagePlan buildCoverageImagePlan(const scene::BakeScene &source, const CoverageBaselineSet &baseline,
      const ReceiverCoveragePlan &plan, const Header &published, const LayerIdentities &identities,
      const CoverageReadyImage &local);
}
