// Prepared once per shaded receiver. Chart storage stays below the shading API.
// Irradiance is linear HDR; the diffuse provider converts it to irradiance / pi.
struct ReceiverLightContext {
  bool charted;
  vec4 chart;
};
ReceiverLightContext prepareReceiverLighting(ShadingReceiver receiver, vec3 normal) {
  vec4 chart = vec4(0);
  bool charted = surfaceIrradiance(receiver.baked.coordinate, normal, receiver.baked.bounds, chart);
  return ReceiverLightContext(charted, chart);
}
// Volume-only receivers (such as water) ask the source for a context instead
// of constructing its chart/probe representation in material code.
ReceiverLightContext prepareVolumeReceiverLighting(ShadingReceiver receiver, vec3 normal) {
  return ReceiverLightContext(false, vec4(0));
}
bool receiverUsesSurfaceLighting(ReceiverLightContext context) { return context.charted; }
ReceiverLighting sampleDiffuseLighting(ShadingReceiver receiver, vec3 normal,
                                      ReceiverLightContext context, bool diffuseRequired) {
  return evaluateReceiverLighting(receiver.position, normal, receiver.geometric,
      context.charted, context.chart.rgb, diffuseRequired, frame.ambientExposure.rgb);
}

vec3 receiverBakedDiffuse(ShadingReceiver receiver, vec3 normal, ReceiverLightContext context, vec3 fallback) {
  vec3 irradiance;
  return context.charted ? context.chart.rgb / Pi
      : bakedIrradiance(receiver.position, normal, irradiance) ? irradiance / Pi : fallback;
}
float receiverSunIrradiance(ReceiverLightContext context) {
  return context.chart.w;
}
