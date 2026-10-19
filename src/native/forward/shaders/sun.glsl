// Cascades 0..3 contain complete frozen geometry; 4..7 contain current casters
// outside that snapshot. Matching lightmaps/probes replace the frozen term.
float sunShadowVisibility(vec3 p, float depth, bool includeStatic) {
  if (frame.target.w == 0u || depth > frame.splits.w)
    return 1;
  int cascade = depth < frame.splits.x ? 0 : depth < frame.splits.y ? 1 : depth < frame.splits.z ? 2 : 3;
  vec4 clip = frame.shadows[cascade] * vec4(p, 1);
  vec3 q = clip.xyz / clip.w;
  if (any(greaterThan(abs(q.xy), vec2(1))) || q.z < 0 || q.z > 1)
    return 1;
  vec2 coord = q.xy * .5 + .5;
  vec2 texel = 1.0 / vec2(textureSize(sunShadow, 0).xy);
  // The caster depth span can include a distant 3D skybox. Convert the
  // world-metric bias with the projection's Z row instead of a fixed clip offset.
  mat4 projection = frame.shadows[cascade];
  float depthScale = length(vec3(projection[0].z, projection[1].z, projection[2].z));
  float reference = q.z - frame.localSettings.z * depthScale;
  float visibility = 0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      vec2 uv = coord + vec2(x, y) * texel;
#ifdef VIEWMODEL_PASS
      // The close-up cascade contains both world occluders and the weapon.
      // Only this receiver pass can sample it; world shadows never see it.
      visibility += texture(sunShadow, vec4(uv, 0, reference));
#else
      float moving = texture(sunShadow, vec4(uv, cascade + 4, reference));
      float frozen = includeStatic ? texture(sunShadow, vec4(uv, cascade, reference)) : 1;
      visibility += min(moving, frozen);
#endif
    }
  return visibility / 9;
}
BrdfLobes sunLightResponse(vec4 coordinate, float unitIrradiance, vec3 p, float depth, vec3 n, vec3 v,
                           vec3 base, float roughness, float metallic) {
  bool baked = frame.sunDirection.w > .5 && coordinate.w > .5;
  float visibility = baked ? clamp(textureLod(surfaceAtlas,
      vec3(coordinate.xy, coordinate.z * SurfaceAtlasLayers + (SurfaceAtlasLayers - 1)), 0).r, 0, 1) : 1;
  bool charted = baked;
  if (!baked && frame.sunDirection.w > .5) {
    float probeVisibility;
    if (bakedSunVisibility(p, n, probeVisibility)) {
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
  vec3 scale = frame.sunRadiance.rgb * sunShadowVisibility(p, depth, !baked);
  return BrdfLobes(response.diffuse * diffuse * scale, response.specular * visibility * scale);
}
vec3 sunLight(vec4 coordinate, float unitIrradiance, vec3 p, float depth, vec3 n, vec3 v,
              vec3 base, float roughness, float metallic) {
  BrdfLobes response = sunLightResponse(coordinate, unitIrradiance, p, depth, n, v, base, roughness, metallic);
  return response.diffuse + response.specular;
}
