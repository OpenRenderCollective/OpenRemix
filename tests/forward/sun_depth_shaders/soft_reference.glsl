#pragma once
// Test-only sparse PCSS candidate; not accepted for renderer use.
// Blocker-search approach: https://developer.download.nvidia.com/shaderlibrary/docs/shadow_PCSS.pdf Coordinates and blocker distances remain in scene units.
// Every tap resolves its own virtual tile; allocation neighbours need not be spatial neighbours.
bool sparseSunDepthSample(vec2 projected, out float depth) {
  vec2 relative = projected / sunDepthSampling.x - vec2(sunDepthGrid.xy);
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
  uvec2 pixel = base + uvec2(fract(projected / sunDepthSampling.x) * float(sunDepthAtlas.y));
  uint index = (page * sunDepthAtlas.x + pixel.y) * sunDepthAtlas.x + pixel.x;
  if (index >= sunDepthValues.length()) return false;
  depth = sunDepthValues[index];
  return !isnan(depth) && !isinf(depth) && depth >= -1;
}
vec2 sunDepthDisc(uint sampleIndex, uint count) {
  float radius = sqrt((float(sampleIndex) + .5) / float(count));
  float angle = float(sampleIndex) * 2.399963229728653;
  return radius * vec2(cos(angle), sin(angle));
}
bool sparseSunDepthSoftVisibility(vec3 position, float angularSin, out float visibility) {
  if (!sparseSunDepthVisibility(position, visibility)) return false;
  if (angularSin <= 0) return true;
  if (isnan(angularSin) || isinf(angularSin) || angularSin >= 1) return false;
  float slope = angularSin / sqrt(1 - angularSin * angularSin);
  vec2 projected = vec2(dot(sunDepthAxisX.xyz, position), dot(sunDepthAxisY.xyz, position));
  float reference = dot(sunDepthAxisZ.xyz, position) - sunDepthSampling.y - sunDepthSampling.w;
  float texel = sunDepthSampling.x / float(sunDepthAtlas.y);
  float searchRadius = max(reference, 0) * slope;
  if (searchRadius <= texel * .5) return true;
  float blockerDepth = 0;
  uint blockers = 0u;
  for (uint i = 0u; i < 9u; ++i) {
    vec2 offset = i == 0u ? vec2(0) : sunDepthDisc(i - 1u, 8u) * searchRadius;
    float depth;
    if (!sparseSunDepthSample(projected + offset, depth)) return false;
    if (depth >= 0 && depth < reference) { blockerDepth += depth; ++blockers; }
  }
  if (blockers == 0u) return true;
  float radius = (reference - blockerDepth / float(blockers)) * slope;
  if (radius <= texel * .5) return true;
  float filtered = 0;
  for (uint i = 0u; i < 16u; ++i) {
    vec2 offset = sunDepthDisc(i, 16u) * radius;
    vec3 samplePosition = position + sunDepthAxisX.xyz * offset.x + sunDepthAxisY.xyz * offset.y;
    float sampleVisibility;
    if (!sparseSunDepthVisibility(samplePosition, sampleVisibility)) return false;
    filtered += sampleVisibility;
  }
  visibility = filtered / 16;
  return true;
}
