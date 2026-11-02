#include "sun-shadows.glsl"
#ifndef VIEWMODEL_PASS
#include "sun-depth.glsl"
#endif
#include "sun-values.glsl"
SunLightSample sampleSunLight(vec4 coordinate, vec4 bounds, float unitIrradiance,
                             vec3 p, float depth, vec3 n, vec3 receiverNormal, bool allowBakedVisibility) {
  bool baked = allowBakedVisibility && frame.sunDirection.w > .5 && coordinate.w > .5;
  float visibility = baked ? clamp(sampleSurfaceAtlas(surfaceFilter(coordinate, bounds),
      coordinate.z * SurfaceAtlasLayers + (SurfaceAtlasLayers - 1)).r, 0, 1) : 1;
  bool charted = baked;
  if (allowBakedVisibility && !FullyMappedReceiver && !baked && frame.sunDirection.w > .5) {
    float probeVisibility;
    if (
#ifndef VIEWMODEL_PASS
        sparseSunDepthVisibility(p, probeVisibility) ||
#endif
        bakedSunVisibility(p, n, probeVisibility)) {
      baked = true;
      visibility = probeVisibility;
    }
  }
  vec3 l = -frame.sunDirection.xyz;
  bool direct = charted && frame.localSettings.w > .5;
  // A truncated SH lobe can ring below the receiver plane. Preserve a finite
  // disc crossing the horizon, while rejecting a wholly back-facing source.
  float receiverCosine = dot(n, l);
  float frontCosine = max(receiverCosine, 0);
  float diffuse = direct ? (receiverCosine + frame.sunRadiance.w > 0 ? unitIrradiance : 0)
                         : frontCosine * visibility;
  vec3 scale = frame.sunRadiance.rgb * sunShadowVisibility(p, depth, !baked, receiverNormal);
  return SunLightSample(l, scale, diffuse, visibility);
}
SunLightSample sampleSunLight(vec4 coordinate, vec4 bounds, float unitIrradiance,
                             vec3 p, float depth, vec3 n, vec3 receiverNormal) {
  return sampleSunLight(coordinate, bounds, unitIrradiance, p, depth, n, receiverNormal, true);
}
// Passes without baked sunlight coverage retain their analytic/CSM policy.
SunLightSample sampleUnbakedSunLight(vec3 p, float depth, vec3 n, vec3 receiverNormal) {
  return sampleSunLight(vec4(0), vec4(0), 0, p, depth, n, receiverNormal, false);
}
LightContribution composeSunLight(SunLightSample lighting, BrdfLobes response) {
  return LightContribution(response.diffuse * lighting.diffuseIrradiance * lighting.radiance,
                            response.specular * lighting.staticVisibility * lighting.radiance);
}
LightContribution shadeStandardSunLight(SunLightSample lighting, vec3 n, vec3 v,
                                        vec3 base, float roughness, float metallic) {
  return composeSunLight(lighting, brdfLobes(base, roughness, metallic, n, v, lighting.direction));
}
