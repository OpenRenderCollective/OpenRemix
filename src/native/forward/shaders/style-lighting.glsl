// Optional sparse RGB SH follows the probe array and any lattice directory in binding 5.
// No extra descriptor or per-style geometry is needed. Frame weights remain
// per submission; shared volume buffers are never changed by a live toggle.
#ifndef STYLE_SCALE
#define STYLE_SCALE(style) frame.styleScales[(style) / 4u][(style) % 4u]
#endif
uint styleWord(uint offset) {
  return bakeRecords[bakeDimensions.w * 16u + bakeDirectoryRecords() + bakeDepthRecords() + bakeTransferRecords() + offset / 4u][offset % 4u];
}
bool styledLightingAvailable() {
#ifdef STYLE_WEIGHTS_ACTIVE
  if (!STYLE_WEIGHTS_ACTIVE) return false;
#endif
  if ((floatBitsToUint(bakeSettings.w) & 1u) == 0u) return false;
  return styleWord(0u) == 0x48535352u && styleWord(1u) == 2u;
}
vec3 styledCoefficient(uint pool, uint record, uint coefficient) {
  if (record == 0u) return vec3(0);
  uint word = styleWord(pool + (record - 1u) * 9u + coefficient);
  float step = uintBitsToFloat(((word >> 27u) + 105u) << 23u);
  return vec3(bitfieldExtract(int(word), 0, 9), bitfieldExtract(int(word), 9, 9),
              bitfieldExtract(int(word), 18, 9)) * step;
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
struct StyledSurfaceFootprint {
  uint page, wide;
  uvec2 lower, upper;
  vec2 fraction;
};
struct StyledSurfaceLayout {
  uint size, page, wide, groups;
};
bool styledSurfaceLayout(vec4 coordinate, out StyledSurfaceLayout layoutInfo) {
  if (coordinate.w < .5 || !styledLightingAvailable()) return false;
  uint size = styleWord(4u), pages = styleWord(5u);
  if (size == 0u || !(coordinate.z >= 0 && coordinate.z < float(pages))) return false;
  // Perspective interpolation can put an integer layer just below itself.
  // Use the same nearest-layer rule as the surface texture array.
  uint page = uint(floor(coordinate.z + .5));
  if (page >= pages) return false;
  layoutInfo = StyledSurfaceLayout(size, page, (size + 15u) / 16u, styleWord(3u));
  return true;
}
StyledSurfaceFootprint styledSurfaceFootprint(vec2 uv, StyledSurfaceLayout layoutInfo) {
  vec2 q = clamp(uv * float(layoutInfo.size) - .5, vec2(0), vec2(layoutInfo.size - 1u));
  uvec2 lower = uvec2(q), upper = min(lower + 1u, uvec2(layoutInfo.size - 1u));
  return StyledSurfaceFootprint(layoutInfo.page, layoutInfo.wide, lower, upper, q - vec2(lower));
}
uvec4 styledSurfaceRecords(uint map, StyledSurfaceFootprint footprint) {
  uvec2 lower = footprint.lower, upper = footprint.upper;
  return uvec4(styledSurfaceRecord(map, footprint.page, lower, footprint.wide),
               styledSurfaceRecord(map, footprint.page, uvec2(upper.x, lower.y), footprint.wide),
               styledSurfaceRecord(map, footprint.page, uvec2(lower.x, upper.y), footprint.wide),
               styledSurfaceRecord(map, footprint.page, upper, footprint.wide));
}
vec3 filteredStyledCoefficient(uint pool, uvec4 records, uint coefficient, vec2 fraction) {
  vec3 row0 = mix(styledCoefficient(pool, records.x, coefficient), styledCoefficient(pool, records.y, coefficient), fraction.x);
  vec3 row1 = mix(styledCoefficient(pool, records.z, coefficient), styledCoefficient(pool, records.w, coefficient), fraction.x);
  return mix(row0, row1, fraction.y);
}
vec3 styledSurfaceIrradiance(vec2 uv, StyledSurfaceLayout layoutInfo, float sh[9]) {
  vec3 result = vec3(0);
  StyledSurfaceFootprint footprint = styledSurfaceFootprint(uv, layoutInfo);
  for (uint group = 0u; group < layoutInfo.groups; ++group) {
    uint entry = 12u + group * 8u;
    uint style = styleWord(entry);
    float scale = STYLE_SCALE(style);
    if (scale == 0) continue;
    uint map = styleWord(entry + 1u), pool = styleWord(entry + 3u);
    uvec4 records = styledSurfaceRecords(map, footprint);
    if (all(equal(records, uvec4(0)))) continue;
    for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
      // Axis-aligned normals have exact-zero SH terms. The packed signed
      // coefficients are finite, so those terms need no sparse reads.
      if (sh[coefficient] != 0)
        result += scale * sh[coefficient] * filteredStyledCoefficient(pool, records, coefficient, footprint.fraction);
    }
  }
  return result; // signed; the receiver clamps only after adding base lighting
}
void addStyledSurfaceBands(vec4 coordinate, inout vec3 bands[4]) {
  StyledSurfaceLayout layoutInfo;
  if (!styledSurfaceLayout(coordinate, layoutInfo)) return;
  StyledSurfaceFootprint footprint = styledSurfaceFootprint(coordinate.xy, layoutInfo);
  for (uint group = 0u; group < layoutInfo.groups; ++group) {
    uint entry = 12u + group * 8u;
    float scale = STYLE_SCALE(styleWord(entry));
    if (scale == 0) continue;
    uvec4 records = styledSurfaceRecords(styleWord(entry + 1u), footprint);
    if (all(equal(records, uvec4(0)))) continue;
    uint pool = styleWord(entry + 3u);
    for (uint coefficient = 0u; coefficient < 4u; ++coefficient)
      bands[coefficient] += scale * filteredStyledCoefficient(pool, records, coefficient, footprint.fraction);
  }
}
