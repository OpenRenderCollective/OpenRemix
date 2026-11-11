#include "standard-local-lighting.glsl"
#include "sun-response.glsl"
#include "standard-direct-lighting.glsl"
#include "standard-reflection-response.glsl"
#ifdef VIEWMODEL_PASS
vec3 standardViewModelReflection(vec3 position, vec3 n, vec3 v, vec3 f0, float roughness,
                                 uint layer, LightingReference reference) {
  // Do not evaluate the material response when this pass has no reflection source.
  if (viewModelReflectionsDisabled()) return vec3(0);
  vec3 radiance = viewModelReflectionRadiance(position, n, v, roughness, layer, reference);
  return radiance * standardReflectionFresnel(f0, roughness, max(dot(n, v), 0));
}
#endif
// Standard material entry point shared by world surfaces and viewmodels.
// Providers own lighting storage; materials supply evaluated Surface values.
ShadingResult shadeStandardSurface(Surface surface, ShadingReceiver receiver) {
  vec3 n = surface.normal;
  vec3 ambient = frame.ambientExposure.rgb;
  ShadingResult shaded = initShadingResult(surface, receiver.opacity);
  shaded.reflectionReference.domain = diffuseReflectionReferenceDomain();
  uint debugView = LightingView;
  if (debugView == ViewBakeCoverage) {
    shaded.color = vec4(receiverBakedCoverage(receiver, n), receiver.opacity);
    return shaded;
  }
  ReceiverLightContext lightingContext = prepareReceiverLighting(receiver, n);
  if (debugView == ViewBakedIrradiance || debugView == ViewIrradianceWithoutNormalMaps) {
    vec3 indirect = receiverBakedDiffuse(receiver, n, lightingContext, ambient);
    shaded.color = vec4(min(max(indirect, vec3(0)), vec3(65000)), receiver.opacity);
    return shaded;
  }
  ReceiverLighting diffuse = sampleDiffuseLighting(receiver, n, lightingContext,
      surface.metallic < 1 && any(greaterThan(surface.color.rgb, vec3(0))));
  diffuse.direct.specular = diffuse.direct.specular && surface.directSpecular;
#if defined(OPENREMIX_SPECIALIZED_LOCAL_RESPONSE) && !defined(LIVE_GI)
  if (debugView >= ViewLocalLightWork) {
    // RGB records local candidates, nonzero responses, and moving-shadow
    // candidates on baked receivers. Blue 240–255 marks a charted fragment.
    vec3 work = vec3(0);
    if (diffuse.direct.diffuse || diffuse.direct.specular)
      accumulateStandardLocalLighting(work, surface, receiver, diffuse.direct, debugView);
    if (debugView == ViewLocalLightMembers) {
      work.b += receiverUsesSurfaceLighting(lightingContext) ? 240 : 0;
      shaded.color = vec4(pow(work / 255.0, vec3(2.2)), receiver.opacity);
      return shaded;
    }
    bool workView = debugView == ViewLocalLightWork || debugView == ViewLocalDirectionalOwnership;
    work = min(work, workView ? vec3(127, 127, 15) : vec3(31));
    work.b += receiverUsesSurfaceLighting(lightingContext) ? (workView ? 240 : 224) : 0;
    // Capture with SDR, gamma 2.2, exposure 1 and the tone curve disabled.
    shaded.color = vec4(pow(work / 255.0, vec3(2.2)), receiver.opacity);
    return shaded;
  }
#endif
  shaded.reflectionReference = diffuseLightingReference(diffuse, shaded.reflectionReference.domain);
  shaded.reflectionReference.luminance = min(shaded.reflectionReference.luminance, 65000);
  bool includeBakedDiffuse = debugView == ViewShaded || debugView == ViewTotalDiffuse;
  vec3 color = includeBakedDiffuse ? composeDiffuseLight(diffuse, surface.color.rgb * (1 - surface.metallic)) : vec3(0);
  accumulateStandardDirectLighting(color, surface, receiver, receiverSunIrradiance(lightingContext),
                                   diffuse.direct, debugView);
  if (debugView == ViewShaded) color += surface.emission;
  shaded.reflectance = mix(vec3(.04), surface.color.rgb, surface.metallic);
#ifdef VIEWMODEL_PASS
  if (debugView == ViewShaded) color += standardViewModelReflection(receiver.position, n, receiver.viewDirection,
      shaded.reflectance, surface.roughness, receiver.reflectionLayer, shaded.reflectionReference);
#elif defined(INLINE_WORLD_REFLECTIONS)
  if (debugView == ViewShaded) {
    vec3 v = receiver.viewDirection;
    ReflectionReceiver reflection = ReflectionReceiver(receiver.position,
        environmentReflectionPosition(receiver.position, n), n, reflect(-v, n),
        surface.roughness, shaded.reflectionReference, receiver.reflectionLayer, receiver.reflectionCandidates);
    color += sampleEnvironmentReflection(reflection) *
        standardReflectionFresnel(shaded.reflectance, surface.roughness, max(dot(n, v), 0));
  }
#endif
  shaded.color = vec4(min(max(color, vec3(0)), vec3(65000)), receiver.opacity);
  shaded.hasReflectance = true;
  return shaded;
}
