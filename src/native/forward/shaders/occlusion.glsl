// Two bits per chart/light pair: unknown=0, blocked=1, clear=2.
// Each unfiltered RGBA16F component
// stores one exact byte (0..255), avoiding normalized decoding or another SSBO.
layout(set = 0, binding = 9) uniform sampler2D staticLightOcclusion;
uint occlusionWord(uint index) {
  uint width = frame.occlusion.z;
  uvec4 bytes = uvec4(texelFetch(staticLightOcclusion, ivec2(index % width, index / width), 0));
  return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}
uint staticLightVisibility(uint chart, uint capturedLight) {
  if (chart == 0xffffffffu || capturedLight == 0u)
    return 0u;
  uint stride = frame.occlusion.x;
  uint light = capturedLight - 1u;
  if (light / 16u >= stride || chart >= frame.occlusion.y)
    return 0u;
  uint index = 2u + chart * stride + light / 16u;
  uint width = frame.occlusion.z;
  vec4 bytes = texelFetch(staticLightOcclusion, ivec2(index % width, index / width), 0);
  return (uint(bytes[(light % 16u) / 4u]) >> ((light % 4u) * 2u)) & 3u;
}

float overflowMaskByte(uint base, uint address) {
  uint word = occlusionWord(base + address / 4u);
  return float((word >> ((address % 4u) * 8u)) & 255u) / 255;
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
  // Match the surface atlas's chart bounds and bilinear texel-centre sampling.
  vec2 uv = clamp((coordinate.xy - bounds.xy) / max(bounds.zw - bounds.xy, vec2(1e-20)), 0, 1);
  vec2 pixel = uv * vec2(size - 1u);
  uvec2 a = uvec2(floor(pixel)), b = min(a + 1u, size - 1u);
  vec2 fraction = fract(pixel);
  float row0 = mix(overflowMaskByte(base, address + a.y * size.x + a.x),
                   overflowMaskByte(base, address + a.y * size.x + b.x), fraction.x);
  float row1 = mix(overflowMaskByte(base, address + b.y * size.x + a.x),
                   overflowMaskByte(base, address + b.y * size.x + b.x), fraction.x);
  visibility = mix(row0, row1, fraction.y);
  return true;
}
