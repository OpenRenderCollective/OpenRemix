#include "shadow-filter.glsl"
#include "cube.glsl"
layout(set = 0, binding = 7) uniform sampler2DArrayShadow staticLocalShadows;
layout(set = 0, binding = 8) uniform sampler2DArrayShadow dynamicLocalShadows;
#ifdef VIEWMODEL_PASS
layout(set = 0, binding = 11) uniform sampler2DArrayShadow viewModelLocalShadows;
#endif

float localShadowVisibility(Light light, vec3 position, vec3 receiverNormal, bool includeStatic) {
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
  float plane = dot(receiverNormal, ray);
  vec3 projected = cubeProjection(ray);
  vec2 receiverPixel = (projected.xy * .5 + .5) * frame.localSettings.x - .5;
  vec2 base = floor(receiverPixel) - 1;
  vec4 weightsX = shadowTentWeights(fract(receiverPixel.x));
  vec4 weightsY = shadowTentWeights(fract(receiverPixel.y));
  float visibility = 0;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      float weight = weightsX[x] * weightsY[y];
      vec2 tap = (base + vec2(x, y) + .5) * (2 / frame.localSettings.x) - 1;
      // Reproject every tap across cube seams rather than clamp the old face.
      vec3 adjacent = cubeProjection(cubeDirection(uint(projected.z), tap));
      // The nearest depth sample lies on a different ray. Comparing its
      // radial depth to the centre ray makes flat receivers shadow themselves
      // at grazing angles. Intersect that actual texel-centre ray with the
      // geometric receiver plane. Normal maps must not tilt this plane.
      vec2 pixel = clamp(floor((adjacent.xy * .5 + .5) * frame.localSettings.x),
                         vec2(0), vec2(frame.localSettings.x - 1));
      vec2 centre = (pixel + .5) * (2 / frame.localSettings.x) - 1;
      vec3 direction = normalize(cubeDirection(uint(adjacent.z), centre));
      float denominator = dot(receiverNormal, direction);
      float receiver = abs(denominator) > 1e-7 ? plane / denominator : -1;
      if (receiver <= 0) { visibility += weight; continue; }
      float reference = clamp((receiver - frame.localSettings.y) / origin.w, 0, 1);
      vec4 coordinate = vec4(adjacent.xy * .5 + .5, float(slot * 6u) + adjacent.z, reference);
      float moving = texture(dynamicLocalShadows, coordinate);
#ifdef VIEWMODEL_PASS
      if ((frame.clusterInfo.w & 1u) != 0u) moving = min(moving, texture(viewModelLocalShadows, coordinate));
#endif
      float fixedVisibility = includeStatic ? texture(staticLocalShadows, coordinate) : 1;
      visibility += weight * min(moving, fixedVisibility);
    }
  // Selection fades shadowing, never the light's radiance or baked visibility.
  return mix(visibility, 1, clamp(light.corners[1].w, 0, 1));
}
