#include "light-sample.glsl"
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

LightContribution localLightResponse(uint index, vec3 p, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                vec4 directIrradiance, BakedReceiver receiver, vec3 dielectricF0, vec3 receiverNormal,
                bool aggregateReceiver) {
  const bool frontFacingOnly = true;
#define LOCAL_LIGHT_MISS return LightContribution(vec3(0), vec3(0))
#include "local-light-query.glsl"
#undef LOCAL_LIGHT_MISS
  return shadeStandardLocalLight(lighting, n, v, base, roughness, metallic, dielectricF0);
}
