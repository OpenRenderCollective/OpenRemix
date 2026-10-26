#version 450
layout(location=0) in vec2 uv;
layout(location=1) in vec4 vertexColor;
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform sampler2D albedo;
#if defined(MASK_WITHIN) || defined(MASK_OVERLAY)
// Masked overlays (remixapi_InstanceInfoMaskEXT, raster/mask.cpp): a group's
// coverage at target resolution, zero or one.
layout(set=1,binding=0) uniform sampler2D coverage;
#endif
#ifdef SPRITE
// Sprite overlays (remixapi_InstanceInfoSpriteEXT): flags in state.w bits
// 5-13; factor holds (1 / softDistance, softFadeStart, softFadeEnd, modulate k).
layout(location=2) in vec3 frame1;
layout(location=3) in vec4 sequence;
layout(location=4) in float weight2;
layout(location=5) flat in vec2 depthScale;
// The frame's opaque depth (RasterRenderer::captureSceneDepth), or any
// texture while SOFT_DEPTH is off.
layout(set=1,binding=0) uniform sampler2D sceneDepth;
#endif
layout(push_constant) uniform Draw {
  mat4 mvp;
  vec4 tint;
  vec4 uvTransform;
  uvec4 state;
  vec4 factor;
} draw;
// Display-encoded colours (gamma 2.2) to linear and back.
vec3 linearColor(vec3 c) {
  return pow(max(c, vec3(0)), vec3(2.2));
}
vec3 displayColor(vec3 c) {
  return pow(max(c, vec3(0)), vec3(1.0 / 2.2));
}
// The texture at p; sRGB-encoded texels (state.w bit 2) decoded to linear.
vec4 albedoTexel(vec2 p) {
  vec4 texel = texture(albedo, p);
  if ((draw.state.w & 4) != 0) texel.rgb = linearColor(texel.rgb);
  return texel;
}
vec4 arg(uint index, vec4 texel, vec4 vertex) {
  if (index == 1) return texel;
  if (index == 2) return vertex;
  if (index == 3) return draw.factor;
  return vec4(1);
}
vec4 combine(uint op, vec4 a, vec4 b) {
  return op == 1 ? a : op == 2 ? b : op == 6 ? min(a+b,vec4(1)) : min(a*b*(op==4||op==7?2:op==5?4:1),vec4(1));
}
// Colour operation 7: see combineColor in the path tracer's scene.glsl.
vec4 combineColor(uint op, vec4 a, vec4 b) {
  return op == 7 ? vec4(mix(a.rgb, min(a.rgb * b.rgb, vec3(1)), a.a), a.a) : combine(op, a, b);
}
#ifdef SPRITE
const vec3 luminance = vec3(0.3, 0.59, 0.11);
#endif
void main() {
  vec4 texel = albedoTexel(uv);
#ifdef SPRITE
  uint sprite = (draw.state.w >> 5) & 0x1ffu;
  if ((sprite & 7u) != 0u) {
    vec4 second = albedoTexel(frame1.xy);
    float w = frame1.z;
    if ((sprite & 4u) != 0u) {
      vec4 k = w < 0.25 ? vec4(0.5 - 2.0 * w, 2.0 * w + 0.5, 0, 0)
             : w < 0.75 ? vec4(0, 1.5 - 2.0 * w, 2.0 * w - 0.5, 0)
                        : vec4(0, 0, 2.5 - 2.0 * w, 2.0 * w - 1.5);
      texel = vec4(texel.r * w + texel.g * k.x + texel.a * k.y + second.g * k.z + second.a * k.w);
    } else if ((sprite & 2u) != 0u) {
      texel = dot(luminance, second.rgb * w) >= dot(luminance, texel.rgb * (1.0 - w)) ? second : texel;
    } else {
      texel = mix(texel, second, w);
    }
  }
  if ((sprite & 0x18u) != 0u) {
    vec4 s0 = albedoTexel(sequence.xy), s1 = albedoTexel(sequence.zw);
    vec4 s = (sprite & 0x20u) != 0u
               ? (dot(luminance, s1.rgb * frame1.z) >= dot(luminance, s0.rgb * (1.0 - frame1.z)) ? s1 : s0)
               : mix(s0, s1, weight2);
    uint mode = (sprite >> 3) & 3u;
    if (mode == 1u) texel = (texel + s) * 0.5;
    else if (mode == 2u) texel.rgb = s.rgb;
    else texel.rgb = mix(texel.rgb, s.rgb, s.a);
  }
#endif
  vec4 vertex = vec4(linearColor(vertexColor.rgb), vertexColor.a);
  uint s = draw.state.x;
#ifdef SPRITE
  // The texture-factor argument has no value here (factor holds the sprite's).
  vec4 a = combineColor((s & 3) | ((s >> 10) & 4), arg((s >> 2) & 3, texel, vertex), arg((s >> 4) & 3, texel, vertex));
  vec4 b = combine(((s >> 6) & 3) | ((s >> 11) & 4), arg((s >> 8) & 3, texel, vertex), arg((s >> 10) & 3, texel, vertex));
  result = vec4(a.rgb, b.a) * draw.tint;
  if ((sprite & 0x40u) != 0u) {
    float scene = depthScale.y / (texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r - depthScale.x);
    float soft = abs(scene - 1.0 / gl_FragCoord.w) * draw.factor.x;
    if (draw.factor.z > draw.factor.y) soft = max(smoothstep(draw.factor.y, draw.factor.z, scene), soft);
    result.a *= clamp(soft, 0.0, 1.0);
  }
  if ((sprite & 0x80u) != 0u) {
    // dst x src x k on display-encoded values (see the extension).
    float k = draw.factor.w;
    vec3 source = clamp(displayColor(texel.rgb) * vertex.rgb * draw.tint.rgb, 0.0, 1.0);
    if ((sprite & 0x100u) != 0u) source = mix(vec3(1.0 / k), source, clamp(texel.a * vertex.a * draw.tint.a, 0.0, 1.0));
    // An encoding target (state.w bit 1) blends display values: the encoding
    // below returns `source`. A linear one: (k source)^2.2 / k.
    result.rgb = (draw.state.w & 2) != 0 ? linearColor(source) : pow(k * source, vec3(2.2)) / k;
  }
#else
  vec4 a = combineColor((s & 3) | ((s >> 10) & 4), arg((s >> 2) & 3, texel, vertex), arg((s >> 4) & 3, texel, vertex));
  vec4 b = combine(((s >> 6) & 3) | ((s >> 11) & 4), arg((s >> 8) & 3, texel, vertex), arg((s >> 10) & 3, texel, vertex));
  result = vec4(a.rgb, b.a) * draw.tint;
#endif
  if ((draw.state.w & 1) != 0) result.a = 1;
  float ref = uintBitsToFloat(draw.state.z);
  uint op = draw.state.y & 0xffffu;
  bool pass = op == 7 || (op == 1 && result.a < ref) || (op == 2 && result.a == ref) ||
    (op == 3 && result.a <= ref) || (op == 4 && result.a > ref) ||
    (op == 5 && result.a != ref) || (op == 6 && result.a >= ref);
  if (!pass) discard;
#ifdef MASK_WITHIN
  // A mask source limited to another group's coverage.
  if (texelFetch(coverage, ivec2(gl_FragCoord.xy), 0).r == 0.0) discard;
#endif
#ifdef MASK_OVERLAY
  // Only inside the group's coverage, or (state.w bit 4) only outside it.
  if ((texelFetch(coverage, ivec2(gl_FragCoord.xy), 0).r > 0.0) == ((draw.state.w & 16) != 0)) discard;
#endif
#ifdef HALO_MASK
  // Silhouette (halo outlines, mask sources): coverage only, after the
  // material's alpha test.
  result = vec4(1);
#else
  // The UNORM target takes display values (gamma 2.2, state.w bit 1); its
  // fixed-function blending operates on them, as the game's did.
  if ((draw.state.w & 2) != 0) result.rgb = displayColor(result.rgb);
#endif
}
