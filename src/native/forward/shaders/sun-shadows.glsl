#include "shadow-filter.glsl"
// Cascades 0..3 contain complete frozen geometry; 4..7 contain current casters
// outside that snapshot. Matching lightmaps/probes replace the frozen term.
float sunShadowVisibilityLayers(vec3 p, float depth, bool includeStatic, bool viewModel) {
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
  vec2 receiverPixel = coord / texel - .5;
  vec2 base = floor(receiverPixel) - 1;
  vec4 weightsX = shadowTentWeights(fract(receiverPixel.x));
  vec4 weightsY = shadowTentWeights(fract(receiverPixel.y));
  float visibility = 0;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      float weight = weightsX[x] * weightsY[y];
      vec2 uv = (base + vec2(x, y) + .5) * texel;
      // The viewmodel has one close-up pair; the world has four cascade pairs.
      uint movingLayer = viewModel ? 1u : uint(cascade + 4);
      uint staticLayer = viewModel ? 0u : uint(cascade);
      float moving = texture(sunShadow, vec4(uv, movingLayer, reference));
      float frozen = includeStatic ? texture(sunShadow, vec4(uv, staticLayer, reference)) : 1;
      visibility += weight * min(moving, frozen);
    }
  return visibility;
}

float sunShadowVisibility(vec3 p, float depth, bool includeStatic) {
#ifdef VIEWMODEL_PASS
  return sunShadowVisibilityLayers(p, depth, includeStatic, true);
#else
  return sunShadowVisibilityLayers(p, depth, includeStatic, false);
#endif
}
