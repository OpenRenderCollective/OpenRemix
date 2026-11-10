#ifndef FORWARD_LIGHTMAP_FILTER_GLSL
#define FORWARD_LIGHTMAP_FILTER_GLSL
layout(constant_id = 6) const bool BicubicLightmaps = false;
struct LightmapFilter {
  vec4 uv; // lower XY, upper XY; each bilinear tap stays inside this chart
  vec2 blend;
};
LightmapFilter lightmapFilter(vec2 uv, vec4 bounds, vec2 size) {
  if (!BicubicLightmaps) return LightmapFilter(vec4(uv, uv), vec2(0));
  vec2 texel = uv * size - .5, base = floor(texel), f = fract(texel);
  vec2 f2 = f * f, f3 = f2 * f;
  vec2 w0 = (1 - 3 * f + 3 * f2 - f3) / 6;
  vec2 w1 = (4 - 6 * f2 + 3 * f3) / 6;
  vec2 w2 = (1 + 3 * f + 3 * f2 - 3 * f3) / 6;
  vec2 w3 = f3 / 6;
  vec2 low = (base - .5 + w1 / (w0 + w1)) / size;
  vec2 high = (base + 1.5 + w3 / (w2 + w3)) / size;
  // Bounds are first/last chart texel centres. Clamping each hardware tap
  // extends this chart's edge without fetching a packed neighbour, even at 1x1.
  return LightmapFilter(vec4(clamp(low, bounds.xy, bounds.zw),
                              clamp(high, bounds.xy, bounds.zw)), w2 + w3);
}
vec2 lightmapTap(LightmapFilter stencil, uint tap) {
  return vec2((tap & 1u) == 0u ? stencil.uv.x : stencil.uv.z,
              (tap & 2u) == 0u ? stencil.uv.y : stencil.uv.w);
}
float lightmapWeight(LightmapFilter stencil, uint tap) {
  vec2 weight = mix(1 - stencil.blend, stencil.blend,
                    bvec2((tap & 1u) != 0u, (tap & 2u) != 0u));
  return weight.x * weight.y;
}
#endif
