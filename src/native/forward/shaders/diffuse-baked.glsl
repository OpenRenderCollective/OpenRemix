#include "diffuse-baked-field.glsl"
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool charted, vec3 chartIrradiance,
                                         bool diffuseRequired, vec3 fallback) {
  return sampleBakedDiffuseField(position, normal, geometric, charted, chartIrradiance,
      diffuseRequired, fallback);
}

// A metal may skip diffuse response but still needs incident lighting for the
// baked reflection source's local reference. Keep its storage query here.
// Compatibility describes the selected source, not receiver availability.
uint diffuseReflectionReferenceDomain() { return Source2StaticLightingReference; }
