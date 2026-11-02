#include "directional-fields.glsl"
#include "probe-batch.glsl"
bool emitterAbovePlane(Light light, vec3 p, vec3 n) {
  if (light.radiance.w <= 0)
    return dot(light.positionRadius.xyz - p, n) + light.positionRadius.w > 0;
  return max(max(dot(light.corners[0].xyz - p, n), dot(light.corners[1].xyz - p, n)),
             dot(light.corners[2].xyz - p, n)) > 0;
}
#include "local-light-values.glsl"
// Custom shading models may retain back-light samples for transmission/wrap.
// The standard model requests its existing front-facing early rejection.
// Read the output only when the query succeeds.
bool sampleLocalLight(uint index, vec3 p, vec3 n, vec4 directIrradiance,
    BakedReceiver receiver, vec3 receiverNormal, bool aggregateReceiver, bool frontFacingOnly, out LocalLightSample sampled) {
#define LOCAL_LIGHT_MISS return false
#include "local-light-query.glsl"
#undef LOCAL_LIGHT_MISS
  sampled = lighting;
  return true;
}
