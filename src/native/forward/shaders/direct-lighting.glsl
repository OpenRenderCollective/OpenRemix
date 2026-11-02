// Direct illumination provider shared by world and viewmodel materials.
// Baked visibility, mixed shadows and analytic specular remain owned by the
// existing sun/local implementations. No exposure or emission belongs here.
vec3 directLightComponent(LightContribution light, uint view) {
  if (view == 2u || view == 7u) return light.diffuse;
  if (view == 3u) return light.specular;
  return light.diffuse + light.specular;
}
void accumulateDirectLighting(inout vec3 color, Surface surface, ShadingReceiver receiver,
                            float bakedSunVisibility, DirectLightingPolicy policy, uint debugView) {
  if (!policy.diffuse && !policy.specular) return;
  vec3 n = surface.normal, v = receiver.viewDirection;
  SunLightSample sunlight = sampleReceiverSunLight(receiver, n, bakedSunVisibility);
  LightContribution sun = shadeStandardSunLight(sunlight, n, v, surface.color.rgb, surface.roughness, surface.metallic);
  color += directLightComponent(selectDirectLight(sun, policy), debugView);
  LocalLightingContext localLighting = prepareLocalLighting(receiver, n, surface.metallic < 1, policy.attribution);
  for (uint i = 0u; i < receiverLocalLightCount(localLighting); ++i) {
    LightContribution local = standardReceiverLocalLight(i, receiver, n, v, surface.color.rgb,
        surface.roughness, surface.metallic, localLighting, vec3(.04));
    color += directLightComponent(selectDirectLight(local, policy), debugView);
  }
}
