// Requires occlusionWord() and frame.occlusion from the binding owner.
float overflowMaskByte(uint base, uint address) {
  uint word = occlusionWord(base + address / 4u);
  return float((word >> ((address % 4u) * 8u)) & 255u) / 255;
}
float overflowMaskTexel(uint base, uint address, uint width, bool tiled, uvec2 pixel) {
  if (!tiled)
    return overflowMaskByte(base, address + pixel.y * width + pixel.x);
  uint tile = (pixel.y / 8u) * ((width + 7u) / 8u) + pixel.x / 8u;
  uint descriptor = occlusionWord(base + address / 4u + tile);
  uint tag = descriptor >> 30u;
  if (tag == 0u)
    return float(descriptor & 255u) / 255;
  uint payload = address + (descriptor & 0x3fffffffu);
  uint index = (pixel.y % 8u) * 8u + pixel.x % 8u;
  if (tag == 1u) {
    uint bits = occlusionWord(base + payload / 4u + index / 32u);
    return float((bits >> (index % 32u)) & 1u);
  }
  return overflowMaskByte(base, payload + index);
}
bool overflowLightVisibility(uint chart, uint capturedLight, vec4 coordinate, vec4 bounds,
                              out float visibility) {
  visibility = 1;
  uint base = frame.occlusion.w;
  if (base == 0u || chart >= frame.occlusion.y || capturedLight == 0u || coordinate.w < .5)
    return false;
  uint first = occlusionWord(base + chart * 2u);
  uint count = occlusionWord(base + chart * 2u + 1u);
  uint low = 0u, high = count, light = capturedLight - 1u;
  while (low < high) {
    uint middle = low + (high - low) / 2u;
    if (occlusionWord(base + first + middle * 4u) < light)
      low = middle + 1u;
    else
      high = middle;
  }
  uint record = base + first + low * 4u;
  if (low == count || occlusionWord(record) != light)
    return false;
  uint address = occlusionWord(record + 1u);
  uvec2 size = uvec2(occlusionWord(record + 2u), occlusionWord(record + 3u));
  bool tiled = (size.x & 0x80000000u) != 0u;
  size.x &= 0x7fffffffu;
  // Match the surface atlas's chart bounds and bilinear texel-centre sampling.
  vec2 uv = clamp((coordinate.xy - bounds.xy) / max(bounds.zw - bounds.xy, vec2(1e-20)), 0, 1);
  vec2 pixel = uv * vec2(size - 1u);
  uvec2 a = uvec2(floor(pixel)), b = min(a + 1u, size - 1u);
  vec2 fraction = fract(pixel);
  float row0 = mix(overflowMaskTexel(base, address, size.x, tiled, a),
                   overflowMaskTexel(base, address, size.x, tiled, uvec2(b.x, a.y)), fraction.x);
  float row1 = mix(overflowMaskTexel(base, address, size.x, tiled, uvec2(a.x, b.y)),
                   overflowMaskTexel(base, address, size.x, tiled, b), fraction.x);
  visibility = mix(row0, row1, fraction.y);
  return true;
}
