// Diffuse transport returns irradiance / pi, before material response.
// The receiver, rather than the material, determines whether surface charts or
// volume probes supply it. Keep this decision shared by world and viewmodels.
struct ReceiverLighting {
  vec3 diffuse;
  bool aggregate;
};
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool charted, vec3 chartIrradiance,
                                         bool diffuseRequired, vec3 fallback) {
  ReceiverLighting lighting = ReceiverLighting(fallback, false);
#ifdef LIVE_GI
  lighting.diffuse = diffuseRequired ? liveDiffuse(position, normal, geometric) : vec3(0);
#else
  if (charted) {
    lighting.diffuse = chartIrradiance / Pi;
    lighting.aggregate = true;
#ifdef ZERO_DIFFUSE_MATERIAL
  } else if (localProbeStencil(position, normal)) {
    lighting.diffuse = vec3(0);
#else
  } else {
    vec3 irradiance;
    if (!bakedIrradiance(position, normal, irradiance)) return lighting;
    lighting.diffuse = irradiance / Pi;
#endif
    lighting.aggregate = true;
  }
#endif
  return lighting;
}
