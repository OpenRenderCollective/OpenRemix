// Source-owned standard-material adapter. Preserve Source 2 miss exits while
// keeping its private receiver representation out of generic material code.
#include "direct-light.glsl"
LightContribution standardReceiverLocalLight(uint ordinal, ShadingReceiver receiver, vec3 normal, vec3 view,
    vec3 base, float roughness, float metallic, LocalLightingContext context, vec3 dielectricF0) {
  return localLightResponse(localLightIndex(context.lights, ordinal), receiver.position, normal, view,
      base, roughness, metallic, context.directIrradiance, receiver.baked, dielectricF0,
      normalize(receiver.geometric), context.aggregate);
}
