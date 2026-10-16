#ifndef FORWARD_MATERIAL_COMMON_GLSL
#define FORWARD_MATERIAL_COMMON_GLSL
vec3 linearColor(vec3 c) {
  return mix(c / 12.92, pow((c + .055) / 1.055, vec3(2.4)), greaterThan(c, vec3(.04045)));
}
vec4 argument(uint i, vec4 texel, vec4 vertex, vec4 factor) {
  return i == 1u ? texel : i == 2u ? vertex : i == 3u ? factor : vec4(1);
}
vec4 combine(uint op, vec4 a, vec4 b) {
  return op == 1u   ? a
         : op == 2u ? b
         : op == 6u ? min(a + b, vec4(1))
                    : min(a * b *
                              (op == 4u || op == 7u ? 2
                               : op == 5u           ? 4
                                                    : 1),
                          vec4(1));
}
vec4 surfaceColorValues(uvec4 state, vec4 factor, vec4 tint, vec4 texel, vec4 vertexColor) {
  vec4 vertex = vec4(linearColor(vertexColor.rgb), vertexColor.a);
  uint s = state.x;
  vec4 a = argument((s >> 2) & 3u, texel, vertex, factor), b = argument((s >> 4) & 3u, texel, vertex, factor);
  uint op = (s & 3u) | ((s >> 10) & 4u);
  vec4 rgb = op == 7u ? vec4(mix(a.rgb, min(a.rgb * b.rgb, vec3(1)), a.a), a.a) : combine(op, a, b);
  vec4 alpha = combine(((s >> 6) & 3u) | ((s >> 11) & 4u), argument((s >> 8) & 3u, texel, vertex, factor),
                       argument((s >> 10) & 3u, texel, vertex, factor));
  return vec4(rgb.rgb, alpha.a) * tint;
}
bool alphaPass(uint op, float ref, float alpha) {
  bool pass = op == 7u || (op == 1u && alpha < ref) || (op == 2u && alpha == ref) ||
              (op == 3u && alpha <= ref) || (op == 4u && alpha > ref) || (op == 5u && alpha != ref) ||
              (op == 6u && alpha >= ref);
  return pass;
}
void emissionLobes(vec4 strength, uvec4 state, vec4 base, vec4 albedo, vec4 emissive, out vec3 emission,
                   out float reflected) {
  reflected = 1;
  if (strength.w <= 0) {
    emission = strength.rgb * emissive.rgb;
    return;
  }
  uint mask = (state.w >> 2u) & 3u;
  vec3 share = (mask == 2u ? vec3(emissive.a) : emissive.rgb) * strength.w;
  if (mask == 1u)
    share *= albedo.a;
  emission = strength.rgb * base.rgb * share;
  reflected = 1 - clamp(dot(share, vec3(1.0 / 3.0)), 0, 1);
}
float mapValue(vec4 texel, vec4 remap, uint bits) {
  uint channel = bits & 7u;
  float value = channel == 4u ? dot(texel.rgb, vec3(.2126, .7152, .0722)) : texel[min(channel, 3u)];
  if ((bits & 8u) != 0u)
    value = 1 - value;
  value = pow(max(value, 0), remap.x) * remap.z + remap.y;
  if ((bits & 16u) != 0u)
    value = sqrt(sqrt(2 / (max(value, 0) + 2)));
  return clamp(value, 0, 1);
}

#endif
