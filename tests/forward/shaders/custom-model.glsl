// Deliberately simple alternate response: consumes evaluated material values.
// This fixture tests source composition, not physical parity with Source 2.
ShadingResult shadeExampleDiffuse(Surface surface, ShadingReceiver receiver) {
  ReceiverLightContext context = prepareReceiverLighting(receiver, surface.normal);
  ReceiverLighting light = sampleDiffuseLighting(receiver, surface.normal, context, true);
  vec3 color = composeDiffuseLight(light, surface.color.rgb);
  LocalLightingContext locals = prepareLocalLighting(receiver, surface.normal, true, light.usesDirectAttribution);
  for (uint i = 0u; i < locals.lights.count; ++i) {
    LocalLightSample source;
    if (sampleReceiverLocalLight(i, receiver, surface.normal, locals, true, source)) {
      BrdfLobes response = BrdfLobes(surface.color.rgb / Pi, vec3(0));
      LightContribution contribution = composeLocalLight(source, response);
      color += contribution.diffuse + contribution.specular;
    }
  }
  ShadingResult result;
  result.color = vec4(color + surface.emission, receiver.opacity);
  result.normalRoughness = vec4(surface.normal, surface.roughness);
  result.reflectance = vec4(0);
  result.irradianceLuma = -1;
  return result;
}
