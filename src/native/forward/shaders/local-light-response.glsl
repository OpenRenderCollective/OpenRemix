#ifndef OPENREMIX_LOCAL_LIGHT_RESPONSE
#define OPENREMIX_LOCAL_LIGHT_RESPONSE
#include "local-light-values.glsl"
#include "local-light-composition.glsl"

// Standard PBR response is separate from light transport and visibility.
LightContribution shadeStandardLocalLight(LocalLightSample lighting, vec3 n, vec3 v,
                                          vec3 base, float roughness, float metallic, vec3 dielectricF0) {
  BrdfLobes response = brdfLobes(base, roughness, metallic, n, v, lighting.direction, dielectricF0);
  // Match the Lambertian response used for aggregate irradiance, preserving the
  // original conditional calculation and early exits in the standard path.
  response.diffuse = lighting.aggregateDiffuse ? base * (1 - metallic) / Pi : response.diffuse;
  return composeLocalLight(lighting, response);
}

#endif
