// Surface charts and volume probes share one diffuse receiver contract.
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool charted, vec3 chartIrradiance,
                                         bool diffuseRequired, vec3 fallback) {
  ReceiverLighting lighting = ReceiverLighting(fallback, analyticDirectLighting(false), false);
  if (charted) {
    lighting.diffuse = chartIrradiance / Pi;
    lighting.direct.attribution = true;
    lighting.reflectionReference = true;
#ifdef ZERO_DIFFUSE_MATERIAL
  } else if (localProbeStencil(position, normal)) {
    lighting.diffuse = vec3(0);
    lighting.direct.attribution = true;
    lighting.reflectionReference = true;
#else
  } else {
    vec3 irradiance;
    if (bakedIrradiance(position, normal, irradiance)) {
      lighting.diffuse = irradiance / Pi;
      lighting.direct.attribution = true;
      lighting.reflectionReference = true;
    }
#endif
  }
  return lighting;
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
