// Optional sparse RGB SH follows the immutable probe array in binding 5.
// No extra descriptor or per-style geometry is needed. Frame weights remain
// per submission; shared volume buffers are never changed by a live toggle.
#ifndef STYLE_SCALE
#define STYLE_SCALE(style) frame.styleScales[(style) / 4u][(style) % 4u]
#endif
uint styleWord(uint offset) {
  return bakeRecords[bakeDimensions.w * 16u + offset / 4u][offset % 4u];
}
bool styledLightingAvailable() {
  if ((uint(bakeSettings.w) & 1u) == 0u) return false;
  return styleWord(0u) == 0x48535352u && styleWord(1u) == 1u;
}
vec3 styledCoefficient(uint pool, uint record, uint coefficient) {
  if (record == 0u) return vec3(0);
  uint first = pool + (record - 1u) * 14u + coefficient * 3u / 2u;
  vec2 a = unpackHalf2x16(styleWord(first));
  vec2 b = unpackHalf2x16(styleWord(first + 1u));
  return (coefficient & 1u) == 0u ? vec3(a, b.x) : vec3(a.y, b);
}
vec3 styledProbeIrradiance(uint index, float sh[9]) {
  vec3 result = vec3(0);
  if (!styledLightingAvailable() || index >= styleWord(6u)) return result;
  for (uint group = 0u; group < styleWord(3u); ++group) {
    uint entry = 12u + group * 8u;
    float scale = STYLE_SCALE(styleWord(entry));
    if (scale == 0) continue;
    uint record = styleWord(styleWord(entry + 2u) + index);
    if (record == 0u) continue;
    uint pool = styleWord(entry + 3u);
    for (uint coefficient = 0u; coefficient < 9u; ++coefficient)
      result += scale * sh[coefficient] * styledCoefficient(pool, record, coefficient);
  }
  return result;
}
uint styledSurfaceRecord(uint map, uint page, uvec2 pixel, uint wide) {
  uint tile = (page * wide + pixel.y / 16u) * wide + pixel.x / 16u;
  uint first = styleWord(map + tile);
  return first == 0u ? 0u : first + (pixel.y % 16u) * 16u + pixel.x % 16u;
}
vec3 styledSurfaceIrradiance(vec4 coordinate, float sh[9]) {
  vec3 result = vec3(0);
  if (coordinate.w < .5 || !styledLightingAvailable()) return result;
  uint size = styleWord(4u), pages = styleWord(5u);
  if (size == 0u || !(coordinate.z >= 0 && coordinate.z < float(pages))) return result;
  // Perspective interpolation can put an integer layer just below itself.
  // Use the same nearest-layer rule as the surface texture array.
  uint page = uint(floor(coordinate.z + .5));
  if (page >= pages) return result;
  vec2 q = clamp(coordinate.xy * float(size) - .5, vec2(0), vec2(size - 1u));
  uvec2 lower = uvec2(q), upper = min(lower + 1u, uvec2(size - 1u));
  vec2 fraction = q - vec2(lower);
  uint wide = (size + 15u) / 16u;
  for (uint group = 0u; group < styleWord(3u); ++group) {
    uint entry = 12u + group * 8u;
    float scale = STYLE_SCALE(styleWord(entry));
    if (scale == 0) continue;
    uint map = styleWord(entry + 1u), pool = styleWord(entry + 3u);
    uint a = styledSurfaceRecord(map, page, lower, wide);
    uint b = styledSurfaceRecord(map, page, uvec2(upper.x, lower.y), wide);
    uint c = styledSurfaceRecord(map, page, uvec2(lower.x, upper.y), wide);
    uint d = styledSurfaceRecord(map, page, upper, wide);
    if ((a | b | c | d) == 0u) continue;
    for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
      vec3 row0 = mix(styledCoefficient(pool, a, coefficient), styledCoefficient(pool, b, coefficient), fraction.x);
      vec3 row1 = mix(styledCoefficient(pool, c, coefficient), styledCoefficient(pool, d, coefficient), fraction.x);
      result += scale * sh[coefficient] * mix(row0, row1, fraction.y);
    }
  }
  return result; // signed; the receiver clamps only after adding base lighting
}
