// Surface charts and volume probes share one diffuse receiver contract.
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool charted, vec3 chartIrradiance,
                                         bool diffuseRequired, vec3 fallback) {
  ReceiverLighting lighting = ReceiverLighting(fallback, false);
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
  return lighting;
}
