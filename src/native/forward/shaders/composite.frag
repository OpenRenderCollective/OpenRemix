#version 450
#extension GL_GOOGLE_include_directive : require
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
layout(set = 0, binding = 6) uniform sampler2D viewModel;
layout(push_constant) uniform Display {
  uint prism;
  uint encodeOutput;
  uint samples;
  uint hasReflections;
  uint background;
  uint gamma22;
  uint hardwareSrgb;
  uint hasViewModel;
  uint hdr10;
  float peakNits;
  float paperWhiteNits;
} display;
layout(location = 0) out vec4 result;
#include "../../postfx/shaders/composite.glsl"
#include "../../pathtrace/shaders/display.glsl"
vec3 tonemap(vec3 color) {
  color = postColor(color, gl_FragCoord.xy / sky.target.xy);
  // Share the path tracer's tone curve and HDR working-space encoding.
  if (display.hdr10 != 0u)
    return displayColor(color, displayHdr10, display.peakNits, display.paperWhiteNits);
  if (display.prism != 0u) color = prismToneMap(color, 1.0);
  if (display.gamma22 != 0u && (display.encodeOutput != 0u || display.hardwareSrgb != 0u)) {
    color = gammaEncodeSafe(color, 2.2);
    // Cancel the attachment's later sRGB encode, keeping UI blending linear.
    if (display.hardwareSrgb != 0u)
      color = mix(color / 12.92, pow(max((color + .055) / 1.055, vec3(0)), vec3(2.4)),
                  greaterThan(color, vec3(.04045)));
  } else if (display.encodeOutput != 0u) {
    color = max(color, vec3(0));
    color = mix(color * 12.92, 1.055 * pow(color, vec3(1.0 / 2.4)) - .055,
                greaterThan(color, vec3(.0031308)));
  }
  return color;
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
    else light += sky.radiance.rgb;
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
    result = vec4(mapped, 1);
    gl_FragDepth = 1;
    return;
  }
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  vec4 color = texelFetch(hdr, pixel, 0);
  vec4 weapon = display.hasViewModel != 0u ? texelFetch(viewModel, pixel, 0) : vec4(0);
  if (color.a <= 0 && weapon.a <= 0)
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
  scene = mediumColor(scene, depth);
  float coverage = weapon.a + color.a * (1 - weapon.a);
  vec3 combined = weapon.rgb + scene * color.a * (1 - weapon.a);
  vec3 mapped = tonemap(combined / max(coverage, 1e-6));
  result = vec4(mapped * coverage, coverage);
  gl_FragDepth = weapon.a > 0 ? 0 : depth;
}
