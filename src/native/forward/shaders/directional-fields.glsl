// Mirrors mixed/directional_fields and directional_field. Missing ownership
// returns false; a known black block returns true with zero unit irradiance.
float directionalFieldValue(uint data, uint encoding, uint width, uvec2 pixel, float sh[9]) {
  uint field;
  if (encoding == 0u) {
    field = pixel.y * width + pixel.x;
  } else {
    uint side = encoding == 2u ? 4u : 8u;
    uint descriptor = occlusionWord(data + (pixel.y / side) * ((width + side - 1u) / side) + pixel.x / side);
    if (descriptor == 0u) return 0;
    data += descriptor & 0x3fffffffu;
    field = (descriptor >> 30u) == 1u ? 0u : (pixel.y % side) * side + pixel.x % side;
  }
  float value = 0;
  for (uint c = 0u; c < 9u; ++c) {
    uint halfIndex = field * 9u + c;
    vec2 coefficients = unpackHalf2x16(occlusionWord(data + halfIndex / 2u));
    value += coefficients[halfIndex & 1u] * sh[c];
  }
  return value;
}
// An immutable record address, not a material/SH sampling context. Keep only
// ownership across filter taps so interpolation retains its original lifetime.
struct SurfaceDirectionalField { uint table, entry; };
bool findSurfaceDirectionalField(uint chart, uint capturedLight, vec4 coordinate,
                                  out SurfaceDirectionalField field) {
  uint table = frame.clusterInfo.z;
  if (table == 0u || capturedLight == 0u || coordinate.w < .5 ||
      occlusionWord(table) != 0x48534452u || occlusionWord(table + 1u) != 1u ||
      chart >= occlusionWord(table + 2u)) return false;
  uint light = capturedLight - 1u;
  uint offsets = table + occlusionWord(table + 5u), entries = table + occlusionWord(table + 6u);
  uint first = occlusionWord(offsets + chart), end = occlusionWord(offsets + chart + 1u), last = end;
  while (first < end) {
    uint middle = first + (end - first) / 2u;
    if (occlusionWord(entries + middle * 8u) < light) first = middle + 1u;
    else end = middle;
  }
  if (first == last) return false;
  uint entry = entries + first * 8u;
  if (occlusionWord(entry) != light) return false;
  field = SurfaceDirectionalField(table, entry);
  return true;
}
float sampleSurfaceDirectionalField(SurfaceDirectionalField field, vec4 coordinate, vec3 normal) {
  uint table = field.table, entry = field.entry;
  uvec2 origin = uvec2(occlusionWord(entry + 1u), occlusionWord(entry + 2u));
  uvec2 size = uvec2(occlusionWord(entry + 3u), occlusionWord(entry + 4u));
  vec2 pixel = clamp(coordinate.xy * vec2(textureSize(surfaceAtlas, 0).xy) - .5 - vec2(origin),
                     vec2(0), vec2(size - 1u));
  uvec2 low = uvec2(floor(pixel)), high = min(low + 1u, size - 1u);
  vec2 fraction = fract(pixel);
  uint encoding = occlusionWord(entry + 5u), data = table + occlusionWord(entry + 6u);
  float sh[9];
  surfaceBasis(normal, sh);
  float a = directionalFieldValue(data, encoding, size.x, low, sh);
  float b = directionalFieldValue(data, encoding, size.x, uvec2(high.x, low.y), sh);
  float c = directionalFieldValue(data, encoding, size.x, uvec2(low.x, high.y), sh);
  float d = directionalFieldValue(data, encoding, size.x, high, sh);
  return mix(mix(a, b, fraction.x), mix(c, d, fraction.x), fraction.y);
}
bool surfaceDirectionalField(uint chart, uint capturedLight, vec4 coordinate, vec3 normal, vec4 bounds,
                              out float value) {
  value = 0;
  SurfaceDirectionalField field;
  if (!findSurfaceDirectionalField(chart, capturedLight, coordinate, field)) return false;
  if (!BicubicLightmaps) {
    value = max(sampleSurfaceDirectionalField(field, coordinate, normal), 0);
    return true;
  }
  LightmapFilter stencil = surfaceFilter(coordinate, bounds);
  for (uint tap = 0u; tap < 4u; ++tap) {
    float sampleValue = sampleSurfaceDirectionalField(field,
        vec4(lightmapTap(stencil, tap), coordinate.zw), normal);
    value += lightmapWeight(stencil, tap) * sampleValue;
  }
  value = max(value, 0);
  return true;
}
