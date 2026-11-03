#include "light-sample.glsl"

LightContribution localLightResponse(uint index, vec3 p, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                vec4 directIrradiance, BakedReceiver receiver, vec3 dielectricF0, vec3 receiverNormal,
                bool aggregateReceiver) {
  const bool frontFacingOnly = true;
#define LOCAL_LIGHT_MISS return LightContribution(vec3(0), vec3(0))
#include "local-light-query.glsl"
#undef LOCAL_LIGHT_MISS
  return shadeStandardLocalLight(lighting, n, v, base, roughness, metallic, dielectricF0);
}
