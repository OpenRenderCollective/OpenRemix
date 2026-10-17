#version 450
#ifdef WATER_VOLUME
#extension GL_GOOGLE_include_directive : require
#define WATER_PARAMETERS_SET 0
#define WATER_PARAMETERS_BINDING 5
#include "../water/parameters.glsl"
#endif
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
vec3 mediumColor(vec3 color, float depth) {
#ifdef WATER_VOLUME
  if (waterFrame.cameraMedium.w > 0) {
    vec2 ndc = gl_FragCoord.xy / sky.target.xy * 2 - 1;
    vec4 point = sky.inverseProjection * vec4(ndc, depth, 1);
    float distance = depth >= 1 ? 1e30 : abs(waterFrame.projection[3][3]) > .5
        ? abs(point.z / point.w) : length(point.xyz / point.w);
    vec3 tint = clamp(waterFrame.cameraMedium.rgb, vec3(1e-4), vec3(1));
    vec3 passed = distance < 1e30 ? pow(tint, vec3(distance / waterFrame.cameraMedium.w)) : vec3(0);
    vec3 direction = normalize(waterFrame.domeUp.xyz);
    vec2 uv = vec2(atan(direction.z, direction.x) / 6.28318530718 + .5,
                   acos(clamp(direction.y, -1, 1)) / 3.14159265359);
    vec3 light = waterFrame.sunFog.rgb;
    if (sky.radiance.w > 0) light += textureLod(dome, uv, 0).rgb * sky.radiance.rgb;
    color = passed * color + (1 - passed) * tint * light;
  }
#endif
  return color;
}
void main() {
  if (display.background != 0u) {
    vec2 ndc = gl_FragCoord.xy / sky.target.xy * 2 - 1;
    vec4 view = sky.inverseProjection * vec4(ndc, .5, 1);
    vec3 direction = normalize(mat3(sky.viewToDome) * (view.xyz / view.w));
    vec2 uv = vec2(atan(direction.z, direction.x) / 6.28318530718 + .5,
                   acos(clamp(direction.y, -1, 1)) / 3.14159265359);
    vec3 mapped = tonemap(mediumColor(textureLod(dome, uv, 0).rgb * sky.radiance.rgb, 1));
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
  vec3 mapped = tonemap(mediumColor(scene, depth));
  if (display.encodeOutput != 0u)
    mapped =
        mix(mapped * 12.92, 1.055 * pow(mapped, vec3(1.0 / 2.4)) - .055, greaterThan(mapped, vec3(.0031308)));
  result = vec4(mapped * color.a, color.a);
  gl_FragDepth = depth;
}
