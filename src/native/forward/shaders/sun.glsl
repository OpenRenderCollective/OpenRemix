#include "sun-shadows.glsl"
#ifndef VIEWMODEL_PASS
#include "sun-depth.glsl"
#endif
BrdfLobes sunLightResponse(vec4 coordinate, vec4 bounds, float unitIrradiance, vec3 p, float depth, vec3 n, vec3 v,
                           vec3 base, float roughness, float metallic, vec3 receiverNormal) {
  bool baked = frame.sunDirection.w > .5 && coordinate.w > .5;
  float visibility = baked ? clamp(sampleSurfaceAtlas(surfaceFilter(coordinate, bounds),
      coordinate.z * SurfaceAtlasLayers + (SurfaceAtlasLayers - 1)).r, 0, 1) : 1;
  bool charted = baked;
  if (!FullyMappedReceiver && !baked && frame.sunDirection.w > .5) {
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
  BrdfLobes response = brdfLobes(base, roughness, metallic, n, v, l);
  bool direct = charted && frame.localSettings.w > .5;
  // A truncated SH lobe can ring below the receiver plane. Preserve a finite
  // disc crossing the horizon, while rejecting a wholly back-facing source.
  float diffuse = direct ? (dot(n, l) + frame.sunRadiance.w > 0 ? unitIrradiance : 0)
                         : max(dot(n, l), 0) * visibility;
  vec3 scale = frame.sunRadiance.rgb * sunShadowVisibility(p, depth, !baked, receiverNormal);
  return BrdfLobes(response.diffuse * diffuse * scale, response.specular * visibility * scale);
}
vec3 sunLight(vec4 coordinate, vec4 bounds, float unitIrradiance, vec3 p, float depth, vec3 n, vec3 v,
              vec3 base, float roughness, float metallic) {
  BrdfLobes response = sunLightResponse(coordinate, bounds, unitIrradiance, p, depth, n, v, base, roughness, metallic, n);
  return response.diffuse + response.specular;
}
