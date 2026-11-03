#ifndef OPENREMIX_SUN_RESPONSE
#define OPENREMIX_SUN_RESPONSE
#include "brdf.glsl"
#include "sun-composition.glsl"
LightContribution shadeStandardSunLight(SunLightSample lighting, vec3 n, vec3 v,
                                        vec3 base, float roughness, float metallic) {
  return composeSunLight(lighting, brdfLobes(base, roughness, metallic, n, v, lighting.direction));
}
#endif
