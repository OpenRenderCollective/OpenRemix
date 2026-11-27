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
    // World and viewmodel receivers can trace unmapped local lights and the sun
    // (set 3 of the cascade receivers' layout holds the ray scene). Which of the
    // two they trace follows the live shadow options, without new pipelines.
    bool rayShadows = false;
    bool sharedCaptures() const { return capturePolicy.transport == probes::CaptureTransport::SharedScene; }
    bool rayDiffuse() const { return diffuse == Diffuse::RayQueries || diffuse == Diffuse::RayIncoming; }
    bool composesBakedDirect() const { return diffuse == Diffuse::RayIncoming; }
    // Ownership policy is separate from the implementation's ray capabilities.
    // New live diffuse techniques extend this query without changing receivers.
    // rayDiffuse traces in the fragment shader; the cascades trace in compute.
    bool cascadeDiffuse() const { return diffuse == Diffuse::SplitCascades; }
    bool liveDiffuse() const { return rayDiffuse() || cascadeDiffuse(); }
    bool bakedDiffuse() const { return diffuse == Diffuse::SurfaceAndProbes; }
    bool bakedAttribution() const { return !liveDiffuse() || composesBakedDirect(); }
    bool operator==(const ResourcePlan &) const = default;
  };
  inline ResourcePlan resolveResources(const Options &options, Capabilities capabilities) {
    ResourcePlan plan;
    // Water queries need the same four-set fragment capability as its ray pass.
    // Fall back coherently rather than leaving one enabled pass on ambient only.
    // The cascades trace in compute and their receivers only read probes, so
    // they need neither fragment ray queries nor the water capability.
    const bool cascades = options.splitCascades && capabilities.rayQueries;
    const bool rayDiffuse = !cascades && options.rtGI && capabilities.rayQueries && capabilities.fragmentGI &&
        (!options.water || capabilities.waterRayQueries);
    const bool liveDiffuse = cascades || rayDiffuse;
    auto request = options;
    // The cascades own the whole diffuse field; composition with a baked direct term is not implemented.
    if (cascades) request.bakedDirectWithLiveGI = false;
    plan.receiverLighting = request.lightingPlan(liveDiffuse);
    if (cascades)
      plan.diffuse = Diffuse::SplitCascades;
    else if (rayDiffuse)
      plan.diffuse = plan.receiverLighting.bakedReceivers ? Diffuse::RayIncoming : Diffuse::RayQueries;
    else if (options.baking.enabled)
      plan.diffuse = Diffuse::SurfaceAndProbes;
    plan.capturePolicy = options.capturePolicy(plan.bakedDiffuse() ? probes::CaptureTransport::SharedScene
                                                                 : probes::CaptureTransport::StandaloneScene);
    plan.captureConfig = options.captureConfiguration();
    plan.screenReflections = options.ssr;
    plan.rayReflections = options.rtReflections && capabilities.rayQueries;
    plan.rayWater = options.water && plan.rayReflections && capabilities.waterRayQueries;
    plan.rayScene = plan.liveDiffuse() || plan.rayReflections;
    // Only the cascade receivers carry the extra set; fragment ray queries need
    // the same device limits as fragment GI.
    plan.rayShadows = cascades && options.rayShadowReceivers() && capabilities.fragmentGI;
    // These world diagnostic shaders return before writing a reflectance
    // guide. Keep their output pipeline independent of reflection captures.
    plan.inlineReflections = !capabilities.forceReflectionResolve && options.debugView == 0 &&
        !plan.screenReflections && !plan.rayReflections && !liveDiffuse && options.baking.reflectionProbes != 0;
    plan.reflectionResolve = !plan.inlineReflections && options.worldReflectionGuides() &&
        (plan.screenReflections || plan.rayReflections || options.baking.reflectionProbes != 0 ||
         // The cascades build from the resolved depth and normal guides.
         cascades);
    return plan;
  }
}
