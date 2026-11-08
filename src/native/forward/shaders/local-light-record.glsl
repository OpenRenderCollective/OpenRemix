#ifndef OPENREMIX_LOCAL_LIGHT_RECORD
#define OPENREMIX_LOCAL_LIGHT_RECORD
// Mirrors LightData in bindings.h. The value type does not bind GPU storage.
struct Light {
  // shaping: inner-cone cosine, focus, influence radius, captured index + 1.
  vec4 positionRadius, radiance, directionCone, shaping;
  vec4 corners[3];
  vec4 responseWeights;
  vec4 clusterBounds;
};

// The response modes mirror LightResponse in light_response.h.
bool localLightUsesBakedDiffuse(Light light) { return light.corners[2].w > .5; }
bool localLightHasSelectedHighlight(Light light) { return light.corners[2].w == 3; }
#endif
