// Standard material response to direct light on world surfaces and viewmodels.
// Baked visibility, mixed shadows and analytic specular remain owned by the
// existing sun/local implementations. No exposure or emission belongs here.
vec3 directLightComponent(LightContribution light, uint view) {
  if (view == ViewAnalyticDiffuse || view == ViewTotalDiffuse) return light.diffuse;
  if (view == ViewDirectSpecular) return light.specular;
  return light.diffuse + light.specular;
}
#ifdef OPENREMIX_SPECIALIZED_LOCAL_RESPONSE
#include "local-accumulation-source.glsl"
#else
void accumulateStandardLocalLighting(inout vec3 color, Surface surface, ShadingReceiver receiver,
                                     DirectLightingPolicy policy, uint debugView) {
  vec3 n = surface.normal, v = receiver.viewDirection;
  LocalLightingContext localLighting = prepareLocalLighting(receiver, n, surface.metallic < 1, policy.attribution);
  for (uint i = 0u; i < receiverLocalLightCount(localLighting); ++i) {
    LightContribution local = standardReceiverLocalLight(i, receiver, n, v, surface.color.rgb,
        surface.roughness, surface.metallic, localLighting, vec3(.04));
    color += directLightComponent(selectDirectLight(local, policy), debugView);
  }
}
#endif
void accumulateStandardDirectLighting(inout vec3 color, Surface surface, ShadingReceiver receiver,
                                      float bakedSunIrradiance, DirectLightingPolicy policy, uint debugView) {
  if (!policy.diffuse && !policy.specular) return;
  vec3 n = surface.normal, v = receiver.viewDirection;
  SunLightSample sunlight = sampleReceiverSunLight(receiver, n, bakedSunIrradiance);
  LightContribution sun = shadeStandardSunLight(sunlight, n, v, surface.color.rgb, surface.roughness, surface.metallic);
  color += directLightComponent(selectDirectLight(sun, policy), debugView);
  accumulateStandardLocalLighting(color, surface, receiver, policy, debugView);
}
