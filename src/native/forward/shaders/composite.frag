#version 450
layout(set = 0, binding = 0) uniform sampler2D hdr;
#ifdef SINGLE_SAMPLE
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
#else
layout(set = 0, binding = 1) uniform sampler2DMS sceneDepth;
#endif
layout(set = 0, binding = 2) uniform sampler2D reflections;
layout(set = 0, binding = 3, std140) uniform Sky {
  mat4 inverseProjection, viewToDome;
  vec4 radiance, target;
} sky;
layout(set = 0, binding = 4) uniform sampler2D dome;
layout(push_constant) uniform Display {
  float exposure;
  uint encodeOutput;
  uint samples;
  uint hasReflections;
  uint background;
} display;
layout(location = 0) out vec4 result;
vec3 tonemap(vec3 color) {
  // Neutral, bounded display transform; HDR exposure/bloom can later use the
  // shared postfx service without changing forward material shading.
  color *= display.exposure;
  return clamp((color * (2.51 * color + .03)) / (color * (2.43 * color + .59) + .14), 0, 1);
}
void main() {
  if (display.background != 0u) {
    vec2 ndc = gl_FragCoord.xy / sky.target.xy * 2 - 1;
    vec4 view = sky.inverseProjection * vec4(ndc, .5, 1);
    vec3 direction = normalize(mat3(sky.viewToDome) * (view.xyz / view.w));
    vec2 uv = vec2(atan(direction.z, direction.x) / 6.28318530718 + .5,
                   acos(clamp(direction.y, -1, 1)) / 3.14159265359);
    vec3 mapped = tonemap(textureLod(dome, uv, 0).rgb * sky.radiance.rgb);
    if (display.encodeOutput != 0u)
      mapped = mix(mapped * 12.92, 1.055 * pow(mapped, vec3(1.0 / 2.4)) - .055,
                   greaterThan(mapped, vec3(.0031308)));
    result = vec4(mapped, 1);
    gl_FragDepth = 1;
    return;
  }
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  vec4 color = texelFetch(hdr, pixel, 0);
  if (color.a <= 0)
    discard; // retain the existing raster sky
  float depth = 1;
#ifdef SINGLE_SAMPLE
  depth = texelFetch(sceneDepth, pixel, 0).r;
#else
  for (uint i = 0u; i < display.samples; ++i)
    depth = min(depth, texelFetch(sceneDepth, pixel, int(i)).r);
#endif
  vec3 scene = color.rgb / max(color.a, 1e-6);
  if (display.hasReflections != 0u) scene += texelFetch(reflections,pixel,0).rgb;
  vec3 mapped = tonemap(scene);
  if (display.encodeOutput != 0u)
    mapped =
        mix(mapped * 12.92, 1.055 * pow(mapped, vec3(1.0 / 2.4)) - .055, greaterThan(mapped, vec3(.0031308)));
  result = vec4(mapped * color.a, color.a);
  gl_FragDepth = depth;
}
