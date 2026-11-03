// Deliberately simple alternate response: consumes evaluated material values.
// This fixture tests source composition, not physical parity with Source 2.
ShadingResult shadeSurface(Surface surface, ShadingReceiver receiver) {
  ReceiverLightContext context = prepareReceiverLighting(receiver, surface.normal);
  ReceiverLighting light = sampleDiffuseLighting(receiver, surface.normal, context, true);
  vec3 color = composeDiffuseLight(light, surface.color.rgb);
  SunLightSample sunlight = sampleReceiverSunLight(receiver, surface.normal, receiverSunVisibility(context));
  LightContribution sun = selectDirectLight(composeSunLight(sunlight, BrdfLobes(surface.color.rgb / Pi, vec3(0))), light.direct);
  color += sun.diffuse + sun.specular;
  LocalLightingContext locals = prepareLocalLighting(receiver, surface.normal, light.direct.diffuse, light.direct.attribution);
  for (uint i = 0u; i < receiverLocalLightCount(locals); ++i) {
    LocalLightSample source;
    if (sampleReceiverLocalLight(i, receiver, surface.normal, locals, true, source)) {
      BrdfLobes response = BrdfLobes(surface.color.rgb / Pi, vec3(0));
      LightContribution contribution = selectDirectLight(composeLocalLight(source, response), light.direct);
      color += contribution.diffuse + contribution.specular;
    }
  }
  ShadingResult result = initShadingResult(surface, receiver.opacity);
  result.color = vec4(color + surface.emission, receiver.opacity);
  return result;
}
