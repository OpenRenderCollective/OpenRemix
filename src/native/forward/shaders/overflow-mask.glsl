// Requires occlusionWord() and frame.occlusion from the binding owner.
#ifndef MASK_PAGE_BINDING
#define MASK_PAGE_BINDING 10
#endif
layout(set = 0, binding = MASK_PAGE_BINDING) uniform sampler2DArray staticVisibilityPages;
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
  uint payload = address + (descriptor & 0x3ffffffcu);
  uint index = (pixel.y % 8u) * 8u + pixel.x % 8u;
  if (tag == 1u) {
    uint bits = occlusionWord(base + payload / 4u + index / 32u);
    return float((bits >> (index % 32u)) & 1u);
  }
  if (tag == 3u) {
    uint mode = descriptor & 3u, bits = 2u + mode, colors = 4u << mode;
    uint bit = index * bits, shift = bit % 32u;
    uint indices = occlusionWord(base + payload / 4u + colors / 4u + bit / 32u) >> shift;
    if (shift + bits > 32u)
      indices |= occlusionWord(base + payload / 4u + colors / 4u + bit / 32u + 1u) << (32u - shift);
    uint color = indices & ((1u << bits) - 1u);
    uint palette = occlusionWord(base + payload / 4u + color / 4u);
    return float((palette >> ((color % 4u) * 8u)) & 255u) / 255;
  }
  return overflowMaskByte(base, payload + index);
}
struct VisibilityField {
  uint address;
  uvec2 size;
  uint layer;
  bool paged, tiled;
};
bool indexedVisibilityField(uint base, uint chart, uint capturedLight, vec4 coordinate,
                            out VisibilityField field) {
  if (base == 0u || chart >= frame.occlusion.y || capturedLight == 0u || coordinate.w < .5)
    return false;
  uint first = occlusionWord(base + chart * 2u);
  uint count = occlusionWord(base + chart * 2u + 1u);
  uint low = 0u, high = count, light = capturedLight - 1u;
  uint record;
  if ((count & 0x80000000u) != 0u) {
    // Optional publication-time directory points to the original records.
    if (light >= (count & 0x7fffffffu)) return false;
    record = occlusionWord(base + first + light);
    if (record == 0u) return false;
    record += base;
  } else {
    while (low < high) {
      uint middle = low + (high - low) / 2u;
      if (occlusionWord(base + first + middle * 4u) < light)
        low = middle + 1u;
      else
        high = middle;
    }
    record = base + first + low * 4u;
    if (low == count || occlusionWord(record) != light)
      return false;
  }
  uint address = occlusionWord(record + 1u);
  uvec2 size = uvec2(occlusionWord(record + 2u), occlusionWord(record + 3u));
  bool paged = (size.x & 0x40000000u) != 0u;
  bool tiled = (size.x & 0x80000000u) != 0u;
  size.x &= 0x3fffffffu;
  uint layer = paged ? size.y >> 16u : 0u;
  if (paged) size.y &= 65535u;
  field = VisibilityField(address, size, layer, paged, tiled);
  return true;
}
float sampleVisibilityField(uint base, VisibilityField field, vec4 coordinate, vec4 bounds) {
  uint address = field.address, layer = field.layer;
  uvec2 size = field.size;
  bool paged = field.paged, tiled = field.tiled;
  // Match the surface atlas's chart bounds and bilinear texel-centre sampling.
  vec2 uv = clamp((coordinate.xy - bounds.xy) / max(bounds.zw - bounds.xy, vec2(1e-20)), 0, 1);
  vec2 pixel = uv * vec2(size - 1u);
  uvec2 a = uvec2(floor(pixel)), b = min(a + 1u, size - 1u);
  vec2 fraction = fract(pixel);
  if (paged) {
    ivec2 origin = ivec2(address & 65535u, address >> 16u);
    float row0 = mix(texelFetch(staticVisibilityPages, ivec3(origin + ivec2(a), layer), 0).r,
                     texelFetch(staticVisibilityPages, ivec3(origin + ivec2(b.x, a.y), layer), 0).r, fraction.x);
    float row1 = mix(texelFetch(staticVisibilityPages, ivec3(origin + ivec2(a.x, b.y), layer), 0).r,
                     texelFetch(staticVisibilityPages, ivec3(origin + ivec2(b), layer), 0).r, fraction.x);
    return mix(row0, row1, fraction.y);
  }
  float row0 = mix(overflowMaskTexel(base, address, size.x, tiled, a),
                   overflowMaskTexel(base, address, size.x, tiled, uvec2(b.x, a.y)), fraction.x);
  float row1 = mix(overflowMaskTexel(base, address, size.x, tiled, uvec2(a.x, b.y)),
                   overflowMaskTexel(base, address, size.x, tiled, b), fraction.x);
  return mix(row0, row1, fraction.y);
}
