// Direct illumination provider shared by world and viewmodel materials.
// Baked visibility, mixed shadows and analytic specular remain owned by the
// existing sun/local implementations. No exposure or emission belongs here.
vec3 directLightComponent(LightContribution light, uint view) {
  if (view == 2u || view == 7u) return light.diffuse;
  if (view == 3u) return light.specular;
  return light.diffuse + light.specular;
}
void accumulateDirectLighting(inout vec3 color, Surface surface, ShadingReceiver receiver,
                            float bakedSunVisibility, bool aggregate, uint debugView) {
  vec3 n = surface.normal, v = receiver.viewDirection;
  SunLightSample sunlight = sampleReceiverSunLight(receiver, n, bakedSunVisibility);
  LightContribution sun = shadeStandardSunLight(sunlight, n, v, surface.color.rgb, surface.roughness, surface.metallic);
  color += directLightComponent(sun, debugView);
  LocalLightingContext localLighting = prepareLocalLighting(receiver, n, surface.metallic < 1, aggregate);
  for (uint i = 0u; i < localLighting.lights.count; ++i) {
    uint index = localLightIndex(localLighting.lights, i);
    LightContribution local = localLightResponse(index, receiver.position, n, v, surface.color.rgb,
        surface.roughness, surface.metallic, localLighting.directIrradiance, receiver.baked, vec3(.04),
        normalize(receiver.geometric), aggregate);
    color += directLightComponent(local, debugView);
  }
}
