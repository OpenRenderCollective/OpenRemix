#ifndef OPENREMIX_LOCAL_LIGHT_COMPOSITION
#define OPENREMIX_LOCAL_LIGHT_COMPOSITION
#include "local-light-values.glsl"
#include "diffuse-residual.glsl"

// Response.diffuse is a weight per unit diffuse irradiance. For an aggregate
// sample it must match the material response applied to the aggregate bake.
// Response.specular includes the analytic receiver cosine (as BrdfLobes does).
LightContribution composeLocalLight(LocalLightSample lighting, BrdfLobes response) {
  vec3 diffuse = diffuseResidual(response.diffuse * lighting.diffuseIrradiance,
                                 lighting.movingVisibility, lighting.aggregateDiffuse);
  vec3 specular = response.specular * lighting.analyticWeight;
  // Highlight selection and moving-caster shadow strength are independent.
  if (lighting.selectedHighlight) specular *= lighting.highlightWeight;
  // Aggregate correction uses baked energy units, without a runtime range fade.
  return LightContribution(diffuse * lighting.radiance * (lighting.aggregateDiffuse ? 1 : lighting.influence),
                            specular * lighting.radiance * (lighting.influence * lighting.movingVisibility));
}
// Models with different analytic and preintegrated diffuse responses can supply
// both. Material authors need no visibility/range/highlight bookkeeping.
LightContribution composeLocalLight(LocalLightSample lighting, BrdfLobes analytic, vec3 bakedDiffuseWeight) {
  analytic.diffuse = lighting.aggregateDiffuse ? bakedDiffuseWeight : analytic.diffuse;
  return composeLocalLight(lighting, analytic);
}

#endif
