#version 450
// Halo outlines (remixapi_InstanceInfoHaloEXT). A group's sources are drawn as
// coverage into a mask (raster.frag, HALO_MASK); the mask is blurred by a
// separable Gaussian, twice; the blurred coverage, in the halo's colour, is
// composited onto the frame where the mask is empty.
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform sampler2D source;
#ifdef HALO_BLUR
layout(push_constant) uniform Blur {
  vec4 area;      // texel centres the halo covers: min.xy, max.xy
  vec2 texel;     // 1 / target size
  vec2 direction; // (1, 0) or (0, 1)
  float sigma;    // texels
  float stride;   // texels between taps; above one only for very wide blurs
  int taps;       // bilinear tap pairs on each side of the centre
} blur;
float coverage(float offset) {
  // Outside the halo's area the targets hold nothing of this group.
  vec2 p = clamp(gl_FragCoord.xy + blur.direction * offset, blur.area.xy, blur.area.zw);
  return textureLod(source, p * blur.texel, 0.0).r;
}
void main() {
  float sum = coverage(0.0), total = 1.0;
  float scale = -0.5 / (blur.sigma * blur.sigma);
  for (int i = 0; i < blur.taps; ++i) {
    // Two neighbouring texels per bilinear fetch.
    float a = float(2 * i + 1) * blur.stride, b = a + blur.stride;
    float wa = exp(a * a * scale), wb = exp(b * b * scale);
    float w = wa + wb;
    if (w < 1e-6) break;
    float offset = (a * wa + b * wb) / w;
    sum += w * (coverage(offset) + coverage(-offset));
    total += 2.0 * w;
  }
  result = vec4(sum / total);
}
#endif
#ifdef HALO_COMPOSITE
layout(set=1,binding=0) uniform sampler2D mask;
layout(push_constant) uniform Composite {
  vec4 color;  // display-referred rgb; a: times the halo is composited
  uint flags;  // 1 additive
} halo;
void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  // Only outside the silhouette (the reference's stencil test).
  if (texelFetch(mask, pixel, 0).r > 0.0) discard;
  float coverage = texelFetch(source, pixel, 0).r;
  if (coverage <= 0.0) discard;
  // Display-referred like the reference's 8-bit targets: the blurred colour
  // is colour * coverage (additive, over black) or the blend from white to
  // the colour (otherwise).
  bool additive = (halo.flags & 1u) != 0u;
  vec3 value = additive ? halo.color.rgb * coverage : mix(vec3(1.0), halo.color.rgb, coverage);
  // Additive: dst + value, color.a times. Otherwise dst * value, color.a times.
  result = vec4(additive ? value * halo.color.a : pow(value, vec3(halo.color.a)), 1.0);
}
#endif
