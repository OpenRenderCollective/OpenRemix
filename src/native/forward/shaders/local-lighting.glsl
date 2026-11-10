#include "local-light-storage.glsl"
#include "baked-storage.glsl"
#include "occlusion.glsl"
#include "local-shadows.glsl"
#include "light-sample.glsl"
#include "light-list.glsl"

// Prepared once for a local-light loop. Baked-channel and cluster layouts are
// private to these queries; the shading model supplies its diffuse-work policy.
struct LocalLightingContext {
  vec4 directIrradiance;
  LocalLightList lights;
  vec3 receiverNormal;
  bool aggregate;
};
LocalLightingContext prepareLocalLighting(ShadingReceiver receiver, vec3 normal,
                                         bool diffuseRequired, bool aggregate) {
  LocalLightList list = localLightsForReceiver(receiver, aggregate);
  bool directChannels = list.count != 0u && receiver.directChannels && !indexedSurfaceDirect() &&
      diffuseRequired && any(notEqual(receiver.baked.directLights, uvec4(0xffffffffu)));
  vec4 directIrradiance = directChannels
      ? surfaceDirectIrradiance(receiver.baked.coordinate, normal, receiver.baked.bounds) : vec4(-1);
  return LocalLightingContext(directIrradiance, list, normalize(receiver.geometric), aggregate);
}
bool sampleReceiverLocalLight(uint ordinal, ShadingReceiver receiver, vec3 normal,
                              LocalLightingContext context, bool frontFacingOnly, out LocalLightSample sampled) {
  return sampleLocalLight(localLightIndex(context.lights, ordinal), receiver.position, normal,
      context.directIrradiance, receiver.baked, context.receiverNormal, context.aggregate,
      frontFacingOnly, sampled);
}

uint receiverLocalLightCount(LocalLightingContext context) { return context.lights.count; }
// The standard model can opt into the source-specific early-exit adapter.
#define OPENREMIX_SPECIALIZED_LOCAL_RESPONSE
