// Standard material entry point shared by world surfaces and viewmodels.
// Providers own lighting storage; materials supply evaluated Surface values.
ShadingResult shadeStandardSurface(Surface surface, ShadingReceiver receiver) {
  vec3 n = surface.normal;
  vec3 ambient = frame.ambientExposure.rgb, irradiance;
  ReceiverLightContext lightingContext = prepareReceiverLighting(receiver, n);
  bool charted = lightingContext.charted;
  ShadingResult shaded;
  shaded.normalRoughness = vec4(n, surface.roughness);
  shaded.reflectance = vec4(0);
  shaded.irradianceLuma = -1;
  uint debugView = LightingView;
  if (debugView == 6u) {
    bool covered = charted || localProbeStencil(receiver.position, n);
    shaded.color = vec4(charted ? vec3(0, 0, 1) : covered ? vec3(0, 1, 0) : vec3(1, 0, 0), receiver.opacity);
    return shaded;
  }
  if (debugView == 1u || debugView == 5u) {
    vec3 indirect = charted ? lightingContext.chart.rgb / Pi
        : bakedIrradiance(receiver.position, n, irradiance) ? irradiance / Pi : ambient;
    shaded.color = vec4(min(max(indirect, vec3(0)), vec3(65000)), receiver.opacity);
    return shaded;
  }
  ReceiverLighting diffuse = sampleDiffuseLighting(receiver, n, lightingContext,
      surface.metallic < 1 && any(greaterThan(surface.color.rgb, vec3(0))));
  if (diffuse.aggregate)
    shaded.irradianceLuma = dot(max(diffuse.diffuse, vec3(0)), vec3(.2126, .7152, .0722));
#ifdef ZERO_DIFFUSE_MATERIAL
  // A metal can skip diffuse material response, but reflection normalization
  // still needs the receiver's incident lighting rather than a zero placeholder.
  if (diffuse.aggregate && !charted) {
    vec3 incident;
    shaded.irradianceLuma = bakedIrradiance(receiver.position, n, incident)
        ? dot(max(incident / Pi, vec3(0)), vec3(.2126, .7152, .0722)) : -1;
  }
#endif
  shaded.irradianceLuma = min(shaded.irradianceLuma, 65000);
  bool includeBakedDiffuse = debugView == 0u || debugView == 7u;
  vec3 color = includeBakedDiffuse ? composeDiffuseLight(diffuse, surface.color.rgb * (1 - surface.metallic)) : vec3(0);
  accumulateDirectLighting(color, surface, receiver, lightingContext.chart.w, diffuse.aggregate, debugView);
  if (debugView == 0u) color += surface.emission;
#ifdef VIEWMODEL_PASS
  if (debugView == 0u) color += viewModelReflection(receiver.position, n, receiver.viewDirection, surface.color.rgb,
      surface.roughness, surface.metallic, receiver.reflectionLayer, shaded.irradianceLuma);
#endif
  shaded.color = vec4(min(max(color, vec3(0)), vec3(65000)), receiver.opacity);
  // The pass adapter stores this guide as RGBA8. Alpha retains ordinary
  // selection or a one-based cubemap layer from the same covered MSAA sample.
  shaded.reflectance = vec4(mix(vec3(.04), surface.color.rgb, surface.metallic),
                            float(255u - receiver.reflectionLayer) / 255);
  return shaded;
}
