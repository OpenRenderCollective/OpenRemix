#include "probe-depth-mapping.glsl"
bool directionalProbeDepth() { return (floatBitsToUint(bakeSettings.w) & 0x80000000u) != 0u; }
uint bakeDepthRecords() { return directionalProbeDepth() ? 1u + bakeDimensions.w * 16u : 0u; }
bool bakedProbeDepthVisibility(uint index, vec3 delta, float minimumVariance, out float visibility) {
  visibility = 1; // GLSL out parameters must also be initialized on unknown/fallback returns.
  if (!directionalProbeDepth()) return false;
  uint base = bakeDimensions.w * 16u + bakeDirectoryRecords();
  uvec4 header = bakeRecords[base];
  float maximum = uintBitsToFloat(header.y), distance = length(delta);
  if (header.x != ProbeDepthSide || header.z != bakeDimensions.w || !(maximum > 0) ||
      isnan(distance) || isinf(distance) || distance > maximum) return false;
  if (distance <= 1e-6) return true;
  vec2 q = probeDepthUv(delta) * float(ProbeDepthSide) - .5;
  ivec2 origin = ivec2(floor(q));
  vec2 fraction = q - vec2(origin), moments = vec2(0);
  for (uint tap = 0u; tap < 4u; ++tap) {
    uvec2 offset = uvec2(tap & 1u, tap >> 1u);
    ivec2 p = probeDepthFold(origin + ivec2(offset));
    uint texel = uint(p.y) * ProbeDepthSide + uint(p.x);
    uint packed = bakeRecords[base + 1u + index * 16u + texel / 4u][texel % 4u];
    vec2 weights = mix(1 - fraction, fraction, vec2(offset));
    moments += unpackHalf2x16(packed) * (weights.x * weights.y);
  }
  moments *= vec2(maximum, maximum * maximum);
  if (distance > moments.x) {
    float variance = max(minimumVariance, moments.y - moments.x * moments.x);
    float difference = distance - moments.x;
    visibility = pow(variance / (variance + difference * difference), 3);
  }
  return true;
}
