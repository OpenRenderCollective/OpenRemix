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
    bool rayReflections = false, rayWater = false;
    bool rayScene = false, reflectionResolve = false;
    bool rayDiffuse() const { return diffuse == Diffuse::RayQueries; }
    bool operator==(const ResourcePlan &) const = default;
  };
  inline ResourcePlan resolveResources(const Options &options, Capabilities capabilities) {
    ResourcePlan plan;
    const bool liveDiffuse = options.rtGI && capabilities.rayQueries && capabilities.fragmentGI;
    plan.diffuse = liveDiffuse ? Diffuse::RayQueries
        : options.baking.enabled ? Diffuse::SurfaceAndProbes : Diffuse::Disabled;
    plan.rayReflections = options.rtReflections && capabilities.rayQueries;
    plan.rayWater = options.water && plan.rayReflections && capabilities.waterRayQueries;
    plan.rayScene = plan.rayDiffuse() || plan.rayReflections;
    // Keep the existing attachment compatibility contract on unsupported ray
    // devices. Removing their placeholder resolve requires a coordinated
    // change to Options::sameTargets and runtime target replacement policy.
    plan.reflectionResolve = options.reflections();
    return plan;
  }
}
