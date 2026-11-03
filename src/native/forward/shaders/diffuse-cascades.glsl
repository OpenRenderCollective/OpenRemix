#include "incoming-lighting.glsl"
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool diffuseRequired, vec3 fallback) {
  // Indirect transport only: no baked direct attribution, so analytic lights
  // stay analytic, and no frozen-reflection reference.
  ReceiverLighting lighting = ReceiverLighting(fallback, analyticDirectLighting(false), false);
  if (diffuseRequired)
    lighting.diffuse = sampleIncomingDiffuse(position, normal, geometric);
  return lighting;
}

float diffuseReferenceLuma(vec3 position, vec3 normal) {
  return -1;
}

// Live cascade energy is not the frozen static capture reference.
uint diffuseReflectionReferenceDomain() { return NoLightingReference; }
