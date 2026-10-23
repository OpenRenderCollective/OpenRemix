#pragma once
// Shared by ordinary Forward+ fragments and isolated GPU lookup tests.
#ifndef SUN_DEPTH_TABLE_BINDING
#define SUN_DEPTH_TABLE_BINDING 20
#endif
#ifndef SUN_DEPTH_VALUES_BINDING
#define SUN_DEPTH_VALUES_BINDING 21
#endif
layout(set = 0, binding = SUN_DEPTH_TABLE_BINDING, std430) readonly buffer SunDepthTable {
  vec4 sunDepthAxisX, sunDepthAxisY, sunDepthAxisZ;
  vec4 sunDepthSampling; // tile width, light-facing Z, far Z, bias; all scene units
  ivec4 sunDepthGrid; // minimum virtual XY, width, height
  uvec4 sunDepthAtlas; // page side, interior side, border, physical tiles per axis
  uvec4 sunDepthCounts; // pages, physical tiles, reserved, reserved
  uint sunDepthDirectory[];
};
layout(set = 0, binding = SUN_DEPTH_VALUES_BINDING, std430) readonly buffer SunDepthValues {
  float sunDepthValues[];
};
bool sparseSunDepthVisibility(vec3 position, out float visibility) {
  visibility = 1;
  if (sunDepthCounts.x == 0u || sunDepthCounts.y == 0u || sunDepthSampling.x <= 0 ||
      any(isnan(position)) || any(isinf(position))) return false;
  vec3 projected = vec3(dot(sunDepthAxisX.xyz, position), dot(sunDepthAxisY.xyz, position),
                        dot(sunDepthAxisZ.xyz, position));
  if (projected.z < sunDepthSampling.y || projected.z > sunDepthSampling.z) return false;
  vec2 virtualPosition = projected.xy / sunDepthSampling.x;
  vec2 relative = virtualPosition - vec2(sunDepthGrid.xy);
  if (any(lessThan(relative, vec2(0))) || any(greaterThanEqual(relative, vec2(sunDepthGrid.zw)))) return false;
  uvec2 tile = uvec2(floor(relative));
  uint directoryIndex = tile.y * uint(sunDepthGrid.z) + tile.x;
  if (directoryIndex >= sunDepthDirectory.length()) return false;
  uint entry = sunDepthDirectory[directoryIndex];
  if (entry == 0u || entry > sunDepthCounts.y) return false;
  uint slot = entry - 1u, perPage = sunDepthAtlas.w * sunDepthAtlas.w;
  uint page = slot / perPage, localSlot = slot % perPage;
  if (page >= sunDepthCounts.x) return false;
  uint extent = sunDepthAtlas.y + 2u * sunDepthAtlas.z;
  uvec2 base = uvec2(localSlot % sunDepthAtlas.w, localSlot / sunDepthAtlas.w) * extent + sunDepthAtlas.z;
  vec2 pixel = vec2(base) + fract(virtualPosition) * float(sunDepthAtlas.y) - .5;
  ivec2 first = ivec2(floor(pixel));
  vec2 fraction = fract(pixel);
  float reference = projected.z - sunDepthSampling.y - sunDepthSampling.w;
  visibility = 0;
  for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
    ivec2 texel = first + ivec2(x, y);
    if (any(lessThan(texel, ivec2(0))) || any(greaterThanEqual(texel, ivec2(sunDepthAtlas.x)))) return false;
    uint index = (page * sunDepthAtlas.x + uint(texel.y)) * sunDepthAtlas.x + uint(texel.x);
    if (index >= sunDepthValues.length()) return false;
    float depth = sunDepthValues[index];
    if (isnan(depth) || isinf(depth) || depth < -1) return false;
    vec2 weight = mix(1 - fraction, fraction, bvec2(x != 0, y != 0));
    visibility += weight.x * weight.y * float(depth < 0 || reference <= depth);
  }
  return true;
}
