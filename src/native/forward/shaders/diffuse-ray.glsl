// Ray-query diffuse implementation. Selected at shader compilation.
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool charted, vec3 chartIrradiance,
                                         bool diffuseRequired, vec3 fallback) {
  ReceiverLighting lighting = ReceiverLighting(fallback, false);
  lighting.diffuse = diffuseRequired ? liveDiffuse(position, normal, geometric) : vec3(0);
  return lighting;
}
