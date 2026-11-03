#include "diffuse-baked-field.glsl"
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool charted, vec3 chartIrradiance,
                                         bool diffuseRequired, vec3 fallback) {
  return sampleBakedDiffuseField(position, normal, geometric, charted, chartIrradiance,
      diffuseRequired, fallback);
}

bool needsDiffuseReferenceQuery(bool hasSurfaceField) {
  return !hasSurfaceField;
}
// A metal may skip diffuse response but still needs incident lighting for the
// baked reflection source's local reference. Keep its storage query here.
float diffuseReferenceLuma(vec3 position, vec3 normal) {
  vec3 incident;
  return bakedIrradiance(position, normal, incident)
      ? dot(max(incident / Pi, vec3(0)), vec3(.2126, .7152, .0722)) : -1;
}

// Compatibility describes the selected source, not receiver availability.
uint diffuseReflectionReferenceDomain() { return Source2StaticLightingReference; }
