#include "cube.glsl"
layout(set = 0, binding = 7) uniform sampler2DArrayShadow staticLocalShadows;
layout(set = 0, binding = 8) uniform sampler2DArrayShadow dynamicLocalShadows;

float localShadowVisibility(Light light, vec3 position, bool includeStatic) {
  uint tag = uint(light.corners[0].w);
  if (tag == 0u)
    return 1;
  uint slot = tag - 1u;
  vec4 origin = frame.localPositions[slot];
  vec3 ray = position - origin.xyz;
  float distance = length(ray);
  // The configured shadow range is independent of the light's full influence.
  if (distance >= origin.w || distance < 1e-8)
    return 1;
  float reference = max(0, (distance - frame.localSettings.y) / origin.w);
  vec3 projected = cubeProjection(ray);
  float visibility = 0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      vec2 tap = projected.xy + vec2(x, y) * (2 / frame.localSettings.x);
      // Reproject every tap across cube seams rather than clamp the old face.
      vec3 adjacent = cubeProjection(cubeDirection(uint(projected.z), tap));
      vec4 coordinate = vec4(adjacent.xy * .5 + .5, float(slot * 6u) + adjacent.z, reference);
      float moving = texture(dynamicLocalShadows, coordinate);
      float fixedVisibility = includeStatic ? texture(staticLocalShadows, coordinate) : 1;
      visibility += min(moving, fixedVisibility);
    }
  // Selection fades shadowing, never the light's radiance or baked visibility.
  return mix(visibility / 9, 1, clamp(light.corners[1].w, 0, 1));
}
