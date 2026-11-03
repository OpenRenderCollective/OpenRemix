#ifndef OPENREMIX_SUN_RESPONSE
#define OPENREMIX_SUN_RESPONSE
// Material response to a sun sample, independent of its transport/storage.
// Requires BrdfLobes and LightContribution, just like local-light response.
#include "sun-values.glsl"
LightContribution composeSunLight(SunLightSample lighting, BrdfLobes response) {
  return LightContribution(response.diffuse * lighting.diffuseIrradiance * lighting.radiance,
                            response.specular * lighting.staticVisibility * lighting.radiance);
}
LightContribution shadeStandardSunLight(SunLightSample lighting, vec3 n, vec3 v,
                                        vec3 base, float roughness, float metallic) {
  return composeSunLight(lighting, brdfLobes(base, roughness, metallic, n, v, lighting.direction));
}
#endif
