#pragma once
#include "forward/probes/selection.h"

namespace remix::forward::probes {
  struct RefinementPolicy {
    uint32_t minimumSamples = 16;
    float minimumSampleFraction = .9f;
    float maximumRelativeUncertainty = .2f;
    // Optional scene-unit detail budget for reassessed diagnostic proposals.
    float maximumAbsoluteUncertainty = 0;
  };
  struct ProjectionRefinement {
    Bounds bounds;
    uint32_t faces = 0; // +X,-X,+Y,-Y,+Z,-Z bit order.
  };
  // A bounded proposal, not a certified surface or accepted capture. Only
  // contracts the existing box; preserve receiver influence separately. Input
  // occupancy must already be validated, and proposals require reassessment.
  std::optional<ProjectionRefinement> refineProjection(const FittedRegion &region, float margin,
                                                       RefinementPolicy policy = {});
}
