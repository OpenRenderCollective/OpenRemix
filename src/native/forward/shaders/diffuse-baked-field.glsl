// Surface charts and volume probes share one diffuse receiver contract.
ReceiverLighting sampleBakedDiffuseField(vec3 position, vec3 normal, vec3 geometric,
                                      bool charted, vec3 chartIrradiance,
                                      bool diffuseRequired, vec3 fallback) {
  ReceiverLighting lighting = ReceiverLighting(fallback, analyticDirectLighting(false), false);
  if (charted) {
    lighting.diffuse = chartIrradiance / Pi;
    lighting.direct.attribution = true;
    lighting.reflectionReference = true;
  } else {
    vec3 irradiance;
    if (bakedIrradiance(position, normal, irradiance)) {
      lighting.diffuse = irradiance / Pi;
      lighting.direct.attribution = true;
      lighting.reflectionReference = true;
    }
  }
  return lighting;
}
