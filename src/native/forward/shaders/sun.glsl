// Cascades 0..3 contain complete frozen geometry; 4..7 contain current casters
// outside that snapshot. Charted receivers can replace only the frozen term.
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
  float visibility = 0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      vec2 uv = coord + vec2(x, y) * texel;
      float moving = texture(sunShadow, vec4(uv, cascade + 4, q.z - .0003));
      float frozen = includeStatic ? texture(sunShadow, vec4(uv, cascade, q.z - .0003)) : 1;
      visibility += min(moving, frozen);
    }
  return visibility / 9;
}
vec3 sunLight(vec4 coordinate, float unitIrradiance, vec3 p, float depth, vec3 n, vec3 v,
              vec3 base, float roughness, float metallic) {
  bool baked = frame.sunDirection.w > .5 && coordinate.w > .5;
  float visibility = baked ? clamp(textureLod(surfaceAtlas,
      vec3(coordinate.xy, coordinate.z * SurfaceAtlasLayers + 19), 0).r, 0, 1) : 1;
  vec3 l = -frame.sunDirection.xyz;
  BrdfLobes response = brdfLobes(base, roughness, metallic, n, v, l);
  bool direct = baked && frame.localSettings.w > .5;
  // A truncated SH lobe can ring below the receiver plane. Preserve a finite
  // disc crossing the horizon, while rejecting a wholly back-facing source.
  float diffuse = direct ? (dot(n, l) + frame.sunRadiance.w > 0 ? unitIrradiance : 0)
                         : max(dot(n, l), 0) * visibility;
  return (response.diffuse * diffuse + response.specular * visibility) * frame.sunRadiance.rgb *
         sunShadowVisibility(p, depth, !baked);
}
