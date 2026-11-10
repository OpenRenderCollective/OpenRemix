// Live diffuse needs its geometric ray origin basis, not surface chart storage.
// Context representation is private to the selected source.
struct ReceiverLightContext {
  vec3 geometric;
};
ReceiverLightContext prepareReceiverLighting(ShadingReceiver receiver, vec3 normal) {
  return ReceiverLightContext(receiver.geometric);
}
ReceiverLightContext prepareVolumeReceiverLighting(ShadingReceiver receiver, vec3 normal) {
  return ReceiverLightContext(receiver.geometric);
}
bool receiverUsesSurfaceLighting(ReceiverLightContext context) { return false; }
ReceiverLighting sampleDiffuseLighting(ShadingReceiver receiver, vec3 normal,
                                      ReceiverLightContext context, bool diffuseRequired) {
  return evaluateReceiverLighting(receiver.position, normal, context.geometric,
      diffuseRequired, frame.ambientExposure.rgb);
}
float receiverSunIrradiance(ReceiverLightContext context) {
  return 0; // no surface-chart unit irradiance in this source
}
// The irradiance view explicitly inspects the baked reference field. Ordinary
// live shading never prepares that chart lookup just to populate its context.
vec3 receiverBakedDiffuse(ShadingReceiver receiver, vec3 normal, ReceiverLightContext context, vec3 fallback) {
  vec4 chart;
  if (surfaceIrradiance(receiver.baked.coordinate, normal, receiver.baked.bounds, chart)) return chart.rgb / Pi;
  vec3 irradiance;
  return bakedIrradiance(receiver.position, normal, irradiance) ? irradiance / Pi : fallback;
}
