// Ray-query diffuse implementation. Selected at shader compilation.
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool diffuseRequired, vec3 fallback) {
  ReceiverLighting lighting = ReceiverLighting(fallback, analyticDirectLighting(false), false);
  lighting.diffuse = diffuseRequired ? liveDiffuse(position, normal, geometric) : vec3(0);
  return lighting;
}

// Live diffuse does not advertise a compatible frozen-reflection reference.
float diffuseReferenceLuma(vec3 position, vec3 normal) {
  return -1;
}

// Compatibility describes the selected source, not receiver availability.
uint diffuseReflectionReferenceDomain() { return NoLightingReference; }
