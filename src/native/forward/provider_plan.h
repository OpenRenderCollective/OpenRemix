#pragma once
#include "forward/techniques.h"

namespace remix::forward::techniques {
  struct Capabilities {
    bool rayQueries = false;
    bool fragmentGI = false;
    bool waterRayQueries = false;
    bool forceReflectionResolve = false;
  };

  // Derived resource/pipeline choices, never independently persisted. Resolve
  // outside draw loops; providers consume this rather than reinterpreting keys.
  struct ResourcePlan {
    Diffuse diffuse = Diffuse::Disabled;
    // Freeze producer and receiver ownership together with device fallback.
    // Refreshed on option updates, not reconstructed during frame recording.
    lighting::Plan receiverLighting;
    // Select the automatic source and admission owner independently of image
    // availability. Disabled captures still visit their owner to retire old work.
    probes::CapturePolicy capturePolicy;
    probes::CaptureConfig captureConfig;
    bool rayReflections = false, rayWater = false;
    bool rayScene = false, reflectionResolve = false;
    bool inlineReflections = false;
    bool screenReflections = false;
    bool sharedCaptures() const { return capturePolicy.transport == probes::CaptureTransport::SharedScene; }
    bool rayDiffuse() const { return diffuse == Diffuse::RayQueries || diffuse == Diffuse::RayIncoming; }
    bool composesBakedDirect() const { return diffuse == Diffuse::RayIncoming; }
    // Ownership policy is separate from the implementation's ray capabilities.
    // New live diffuse techniques extend this query without changing receivers.
    bool liveDiffuse() const { return rayDiffuse(); }
    bool bakedDiffuse() const { return diffuse == Diffuse::SurfaceAndProbes; }
    bool bakedAttribution() const { return !liveDiffuse() || composesBakedDirect(); }
    bool operator==(const ResourcePlan &) const = default;
  };
  inline ResourcePlan resolveResources(const Options &options, Capabilities capabilities) {
    ResourcePlan plan;
    // Water queries need the same four-set fragment capability as its ray pass.
    // Fall back coherently rather than leaving one enabled pass on ambient only.
    const bool liveDiffuse = options.rtGI && capabilities.rayQueries && capabilities.fragmentGI &&
        (!options.water || capabilities.waterRayQueries);
    plan.receiverLighting = options.lightingPlan(liveDiffuse);
    if (liveDiffuse)
      plan.diffuse = plan.receiverLighting.bakedReceivers ? Diffuse::RayIncoming : Diffuse::RayQueries;
    else if (options.baking.enabled)
      plan.diffuse = Diffuse::SurfaceAndProbes;
    plan.capturePolicy = options.capturePolicy(plan.bakedDiffuse() ? probes::CaptureTransport::SharedScene
                                                                 : probes::CaptureTransport::StandaloneScene);
    plan.captureConfig = options.captureConfiguration();
    plan.screenReflections = options.ssr;
    plan.rayReflections = options.rtReflections && capabilities.rayQueries;
    plan.rayWater = options.water && plan.rayReflections && capabilities.waterRayQueries;
    plan.rayScene = plan.rayDiffuse() || plan.rayReflections;
    // These world diagnostic shaders return before writing a reflectance
    // guide. Keep their output pipeline independent of reflection captures.
    plan.inlineReflections = !capabilities.forceReflectionResolve && options.debugView == 0 &&
        !plan.screenReflections && !plan.rayReflections && !liveDiffuse && options.baking.reflectionProbes != 0;
    plan.reflectionResolve = !plan.inlineReflections && options.worldReflectionGuides() &&
        (plan.screenReflections || plan.rayReflections || options.baking.reflectionProbes != 0);
    return plan;
  }
}
