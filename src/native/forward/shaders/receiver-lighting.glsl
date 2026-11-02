// Diffuse transport returns irradiance / pi, before material response.
// The receiver, rather than the material, determines whether surface charts or
// volume probes supply it. Keep this decision shared by world and viewmodels.
struct ReceiverLighting {
  vec3 diffuse;
  bool aggregate;
};
// Select implementations at compilation; materials never branch on provider IDs.
#ifdef LIVE_GI
#include "diffuse-ray.glsl"
#else
#include "diffuse-baked.glsl"
#endif

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
ReceiverLighting sampleDiffuseLighting(ShadingReceiver receiver, vec3 normal,
                                      ReceiverLightContext context, bool diffuseRequired) {
  return evaluateReceiverLighting(receiver.position, normal, receiver.geometric,
      context.charted, context.chart.rgb, diffuseRequired, frame.ambientExposure.rgb);
}

// ReceiverLighting.diffuse already contains irradiance / pi. The material
// supplies diffuse reflectance (for standard PBR: base color * (1 - metalness)).
vec3 composeDiffuseLight(ReceiverLighting lighting, vec3 diffuseReflectance) {
  return diffuseReflectance * lighting.diffuse;
}

// These diagnostics intentionally inspect the baked reference field even when
// live diffuse transport is selected. Keep that policy out of material models.
vec3 receiverBakedCoverage(ShadingReceiver receiver, vec3 normal, ReceiverLightContext context) {
  bool covered = context.charted || localProbeStencil(receiver.position, normal);
  return context.charted ? vec3(0, 0, 1) : covered ? vec3(0, 1, 0) : vec3(1, 0, 0);
}
vec3 receiverBakedDiffuse(ShadingReceiver receiver, vec3 normal, ReceiverLightContext context, vec3 fallback) {
  vec3 irradiance;
  return context.charted ? context.chart.rgb / Pi
      : bakedIrradiance(receiver.position, normal, irradiance) ? irradiance / Pi : fallback;
}
// Incident lighting is also needed for reflection normalization on metals,
// whose diffuse material response can otherwise skip the probe evaluation.
float receiverBakedIrradianceLuma(ShadingReceiver receiver, vec3 normal) {
  vec3 incident;
  return bakedIrradiance(receiver.position, normal, incident)
      ? dot(max(incident / Pi, vec3(0)), vec3(.2126, .7152, .0722)) : -1;
}
float receiverSunVisibility(ReceiverLightContext context) {
  return context.chart.w;
}
