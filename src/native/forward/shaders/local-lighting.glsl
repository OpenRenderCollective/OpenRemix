#include "baked-storage.glsl"
#include "occlusion.glsl"
#include "local-shadows.glsl"
#include "direct-light.glsl"
#include "light-list.glsl"

// Prepared once for a local-light loop. Baked-channel and cluster layouts are
// private to these queries; the shading model supplies its diffuse-work policy.
struct LocalLightingContext {
  vec4 directIrradiance;
  LocalLightList lights;
  bool aggregate;
};
LocalLightingContext prepareLocalLighting(ShadingReceiver receiver, vec3 normal,
                                         bool diffuseRequired, bool aggregate) {
  bool directChannels = receiver.directChannels && !indexedSurfaceDirect() && diffuseRequired &&
      frame.target.z > 0u && any(notEqual(receiver.baked.directLights, uvec4(0xffffffffu)));
  vec4 directIrradiance = directChannels
      ? surfaceDirectIrradiance(receiver.baked.coordinate, normal, receiver.baked.bounds) : vec4(-1);
  return LocalLightingContext(directIrradiance, localLightsForReceiver(receiver, aggregate), aggregate);
}
bool sampleReceiverLocalLight(uint ordinal, ShadingReceiver receiver, vec3 normal,
                              LocalLightingContext context, bool frontFacingOnly, out LocalLightSample sampled) {
  return sampleLocalLight(localLightIndex(context.lights, ordinal), receiver.position, normal,
      context.directIrradiance, receiver.baked, normalize(receiver.geometric), context.aggregate,
      frontFacingOnly, sampled);
}

uint receiverLocalLightCount(LocalLightingContext context) { return context.lights.count; }
// Preserve the specialized miss exits in the default source. Other sources
// can use the shared standard response adapter without implementing a BRDF.
#define OPENREMIX_SPECIALIZED_LOCAL_RESPONSE
// Standard response adapter: source indices, direct channels and shadow inputs
// stay private to the source. Custom models use sampleReceiverLocalLight instead.
LightContribution standardReceiverLocalLight(uint ordinal, ShadingReceiver receiver, vec3 normal, vec3 view,
    vec3 base, float roughness, float metallic, LocalLightingContext context, vec3 dielectricF0) {
  return localLightResponse(localLightIndex(context.lights, ordinal), receiver.position, normal, view,
      base, roughness, metallic, context.directIrradiance, receiver.baked, dielectricF0,
      normalize(receiver.geometric), context.aggregate);
}
