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
// The standard model can opt into the source-specific early-exit adapter.
#define OPENREMIX_SPECIALIZED_LOCAL_RESPONSE
