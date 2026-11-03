#pragma once
#include "forward/bake/config.h"

namespace remix::forward::lighting {
  struct Request {
    bool baked = false;
    bool selectedMixedLights = false;
    bool composeBakedDirect = false;
  };
  struct Plan {
    bake::Config baking;
    bool bakedReceivers = false;
    bool selectedMixedLights = false;
    bool operator==(const Plan &) const = default;
  };

  // Resolve direct and incoming ownership once. Requested settings remain
  // untouched; combined bakes are never added to a live incoming field.
  inline Plan resolve(bake::Config baking, Request request, bool liveGiActive) {
    Plan plan{baking};
    plan.bakedReceivers = request.baked && baking.enabled && baking.directDiffuse &&
        (!liveGiActive || request.composeBakedDirect);
    plan.selectedMixedLights = plan.bakedReceivers || (!liveGiActive && request.selectedMixedLights);
    if (plan.bakedReceivers) {
      plan.baking.aggregateDiffuse = true;
      plan.baking.fullyBakedLights = false;
      plan.baking.bakedGeometryLights = true;
      plan.baking.groupedStyledDirect = true;
    }
    if (liveGiActive && plan.bakedReceivers) {
      plan.baking.irradianceContents = bake::IrradianceContents::ReceiverDirect;
      plan.baking.bounces = 1; // no incoming paths are traced by this producer
      plan.baking.styledIndirect = false; // live incoming owns these components
      // Capture quality belongs to the independent complete-radiance owner;
      // canonicalize unused fields so its edits cannot invalidate direct data.
      plan.baking.reflectionProbes = 0;
      plan.baking.reflectionResolution = 8; // valid empty-capture filter configuration
      plan.baking.reflectionDetailTolerance = 0;
    } else if (liveGiActive) {
      // The selected live provider replaces this pipeline; independent cubemap
      // capture uses the reflection-only scheduler instead of baking unused fields.
      plan.baking.enabled = false;
      plan.baking.groupedStyledDirect = false;
      plan.baking.aggregateDiffuse = false;
      plan.baking.fullyBakedLights = false;
      plan.baking.bakedGeometryLights = false;
    }
    return plan;
  }
}
