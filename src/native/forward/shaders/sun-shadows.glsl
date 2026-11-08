#include "shadow-filter.glsl"
#ifndef SUN_SHADOW_BINDING
#define SUN_SHADOW_BINDING 4
#endif
layout(set = 0, binding = SUN_SHADOW_BINDING) uniform sampler2DArray sunShadow;
const uint MovingSunCoverageFlag = 1u << 2;
vec4 sunShadowBlock(vec2 pixel, vec2 texel, vec2 coord, vec2 slope, float depth, float bias,
                    float movingLayer, float staticLayer, bool includeStatic) {
  vec2 centre = (pixel + 1) * texel;
  vec4 stored = textureGather(sunShadow, vec3(centre, movingLayer)).wzxy;
  if (includeStatic)
    stored = min(stored, textureGather(sunShadow, vec3(centre, staticLayer)).wzxy);
  // The clamp-to-edge gather repeats border texels. Evaluate the receiver
  // plane at those same clamped centres, with a distinct depth per tap.
  vec2 low = clamp((pixel + .5) * texel, texel * .5, 1 - texel * .5);
  vec2 high = clamp((pixel + 1.5) * texel, texel * .5, 1 - texel * .5);
  vec2 dx = (vec2(low.x, high.x) - coord.x) * 2;
  vec2 dy = (vec2(low.y, high.y) - coord.y) * 2;
  vec4 reference = depth + (dx.xyxy * slope.x + dy.xxyy * slope.y) - bias;
  return vec4(lessThanEqual(reference, stored));
}
// Cascades 0..3 contain complete frozen geometry; 4..7 contain current casters
// outside that snapshot. Matching lightmaps/probes replace the frozen term.
float sunShadowVisibilityLayers(vec3 p, float depth, bool includeStatic, bool viewModel, vec3 receiverNormal) {
  if (frame.target.w == 0u || depth > frame.splits.w)
    return 1;
  int cascade = depth < frame.splits.x ? 0 : depth < frame.splits.y ? 1 : depth < frame.splits.z ? 2 : 3;
  vec4 clip = frame.shadows[cascade] * vec4(p, 1);
  vec3 q = clip.xyz / clip.w;
  if (any(greaterThan(abs(q.xy), vec2(1))) || q.z < 0 || q.z > 1)
    return 1;
  vec2 coord = q.xy * .5 + .5;
  vec2 texel = 1.0 / vec2(textureSize(sunShadow, 0).xy);
  if (!includeStatic && (frame.clusterInfo.w & MovingSunCoverageFlag) != 0u) {
    // The moving map is clear outside submitted caster bounds. Include the
    // full 4x4 tent footprint; frozen fallback maps remain independent.
    vec4 coverage = frame.movingSunCoverage[cascade];
    vec2 margin = 4 * texel + 1e-4;
    if (any(greaterThan(coverage.xy, coverage.zw)) ||
        any(lessThan(q.xy, coverage.xy - margin)) || any(greaterThan(q.xy, coverage.zw + margin)))
      return 1;
  }
  // The caster depth span can include a distant 3D skybox. Convert the
  // world-metric bias with the projection's Z row instead of a fixed clip offset.
  mat4 projection = frame.shadows[cascade];
  float depthScale = length(vec3(projection[0].z, projection[1].z, projection[2].z));
  float bias = frame.localSettings.z * depthScale;
  // Compare each nearest texel against the same geometric receiver plane.
  // A centre-depth comparison makes sloped triangles shadow themselves.
  vec3 tangent = cross(receiverNormal, abs(receiverNormal.z) < .9 ? vec3(0, 0, 1) : vec3(0, 1, 0));
  vec3 a = mat3(projection) * tangent;
  vec3 b = mat3(projection) * cross(receiverNormal, tangent);
  float determinant = a.x * b.y - a.y * b.x;
  vec2 slope = abs(determinant) > 1e-6 * max(length(a.xy) * length(b.xy), 1e-30)
      ? vec2(a.z * b.y - b.z * a.y, a.x * b.z - b.x * a.z) / determinant : vec2(0);
  vec2 receiverPixel = coord / texel - .5;
  vec2 base = floor(receiverPixel) - 1;
  vec4 weightsX = shadowTentWeights(fract(receiverPixel.x));
  vec4 weightsY = shadowTentWeights(fract(receiverPixel.y));
  // The viewmodel has one close-up pair; the world has four cascade pairs.
  float movingLayer = viewModel ? 1 : float(cascade + 4);
  float staticLayer = viewModel ? 0 : float(cascade);
  float visibility = 0;
  for (int y = 0; y < 4; y += 2) {
    vec4 left = sunShadowBlock(base + vec2(0, y), texel, coord, slope, q.z, bias,
                               movingLayer, staticLayer, includeStatic);
    vec4 right = sunShadowBlock(base + vec2(2, y), texel, coord, slope, q.z, bias,
                                movingLayer, staticLayer, includeStatic);
    visibility += dot(weightsX * weightsY[y], vec4(left.xy, right.xy));
    visibility += dot(weightsX * weightsY[y + 1], vec4(left.zw, right.zw));
  }
  return visibility;
}

float sunShadowVisibility(vec3 p, float depth, bool includeStatic, vec3 receiverNormal) {
#ifdef VIEWMODEL_PASS
  return sunShadowVisibilityLayers(p, depth, includeStatic, true, receiverNormal);
#else
  return sunShadowVisibilityLayers(p, depth, includeStatic, false, receiverNormal);
#endif
}
