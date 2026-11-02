#include "diffuse-values.glsl"
// Scene-linear radiance after source visibility and material response. Unlike
// BrdfLobes, these values can be accumulated directly into the shaded result.
// Diffuse can be a negative mixed-shadow correction; clamp only the final sum.
struct LightContribution {
  vec3 diffuse;
  vec3 specular;
};

// Source ownership selects incident lobes before diagnostic/material summation.
LightContribution selectDirectLight(LightContribution light, DirectLightingPolicy policy) {
  return LightContribution(policy.diffuse ? light.diffuse : vec3(0),
                           policy.specular ? light.specular : vec3(0));
}
