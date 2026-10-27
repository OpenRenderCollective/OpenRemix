#pragma once
#include "forward/bake/config.h"

namespace remix::forward::lighting {
  struct Request {
    bool baked = false;
    bool selectedMixedLights = false;
  };
  struct Plan {
    bake::Config baking;
    bool bakedReceivers = false;
    bool selectedMixedLights = false;
  };

  // Resolve ownership once: live GI replaces aggregate irradiance rather than
  // adding another diffuse contribution. Requested settings remain untouched.
  inline Plan resolve(bake::Config baking, Request request, bool liveGiActive) {
    Plan plan{baking};
    plan.bakedReceivers = request.baked && baking.enabled && baking.directDiffuse && !liveGiActive;
    plan.selectedMixedLights = !liveGiActive && (request.selectedMixedLights || plan.bakedReceivers);
    if (plan.bakedReceivers) {
      plan.baking.aggregateDiffuse = true;
      plan.baking.fullyBakedLights = false;
      plan.baking.bakedGeometryLights = true;
      plan.baking.groupedStyledDirect = true;
    }
    if (liveGiActive) {
      plan.baking.groupedStyledDirect = false;
      plan.baking.aggregateDiffuse = false;
      plan.baking.fullyBakedLights = false;
      plan.baking.bakedGeometryLights = false;
    }
    return plan;
  }
}
