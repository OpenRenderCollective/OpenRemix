#pragma once
#include "forward/techniques.h"

namespace remix::forward::techniques {
  struct Capabilities {
    bool rayQueries = false;
    bool fragmentGI = false;
    bool waterRayQueries = false;
  };

  // Derived resource/pipeline choices, never independently persisted. Resolve
  // outside draw loops; providers consume this rather than reinterpreting keys.
  struct ResourcePlan {
    Diffuse diffuse = Diffuse::Disabled;
    // Select the automatic source and admission owner independently of image
    // availability. Disabled captures still visit their owner to retire old work.
    probes::CapturePolicy capturePolicy;
    bool rayReflections = false, rayWater = false;
    bool rayScene = false, reflectionResolve = false;
    bool screenReflections = false;
    bool sharedCaptures() const { return capturePolicy.transport == probes::CaptureTransport::SharedScene; }
    bool rayDiffuse() const { return diffuse == Diffuse::RayQueries; }
    // Ownership policy is separate from the implementation's ray capabilities.
    // New live diffuse techniques extend this query without changing receivers.
    bool liveDiffuse() const { return rayDiffuse(); }
    bool bakedDiffuse() const { return diffuse == Diffuse::SurfaceAndProbes; }
    bool operator==(const ResourcePlan &) const = default;
  };
  inline ResourcePlan resolveResources(const Options &options, Capabilities capabilities) {
    ResourcePlan plan;
    // Water queries need the same four-set fragment capability as its ray pass.
    // Fall back coherently rather than leaving one enabled pass on ambient only.
    const bool liveDiffuse = options.rtGI && capabilities.rayQueries && capabilities.fragmentGI &&
        (!options.water || capabilities.waterRayQueries);
    plan.diffuse = liveDiffuse ? Diffuse::RayQueries
        : options.baking.enabled ? Diffuse::SurfaceAndProbes : Diffuse::Disabled;
    plan.capturePolicy = options.capturePolicy(plan.bakedDiffuse() ? probes::CaptureTransport::SharedScene
                                                                 : probes::CaptureTransport::StandaloneScene);
    plan.screenReflections = options.ssr;
    plan.rayReflections = options.rtReflections && capabilities.rayQueries;
    plan.rayWater = options.water && plan.rayReflections && capabilities.waterRayQueries;
    plan.rayScene = plan.rayDiffuse() || plan.rayReflections;
    // Allocate guides/resolve only for a supported active reflection source.
    plan.reflectionResolve = plan.screenReflections || plan.rayReflections || options.baking.reflectionProbes != 0;
    return plan;
  }
}
