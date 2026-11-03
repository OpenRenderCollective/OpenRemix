// Default material adapter for any local source exposing the receiver queries.
// Source 2 supplies a specialized version to retain its original miss exits.
#include "local-light-response.glsl"
#ifdef OPENREMIX_SPECIALIZED_LOCAL_RESPONSE
#include "local-response-source.glsl"
#else
LightContribution standardReceiverLocalLight(uint ordinal, ShadingReceiver receiver, vec3 normal, vec3 view,
    vec3 base, float roughness, float metallic, LocalLightingContext context, vec3 dielectricF0) {
  LocalLightSample lighting;
  if (!sampleReceiverLocalLight(ordinal, receiver, normal, context, true, lighting))
    return LightContribution(vec3(0), vec3(0));
  return shadeStandardLocalLight(lighting, normal, view, base, roughness, metallic, dielectricF0);
}
#endif
