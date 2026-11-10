// Shared local-to-dome sampling and visibility-filtered sky fallback.
#include "../../pathtrace/shaders/color.glsl"
vec3 sampleDomeRadiance(vec3 domeDirection) {
  vec3 local = normalize(domeDirection);
  vec2 uv = vec2(atan(local.z, local.x) / 6.28318530718 + .5,
                 acos(clamp(local.y, -1, 1)) / 3.14159265359);
  return decodeColorTexel(textureLod(skyMap, uv, 0), sky.target.w > .5).rgb * sky.radiance.rgb;
}
vec3 environmentReflection(vec3 position, vec3 normal, vec3 reflected, float roughness) {
  if (sky.radiance.w == 0 || all(equal(sky.radiance.rgb, vec3(0))))
    return vec3(0);
  float visibility = bakedSkyVisibility(position, normal, reflected);
  if (visibility <= 0)
    return vec3(0);
  mat3 worldToDome = mat3(sky.viewToDome) * mat3(frame.view);
  if (roughness < .025)
    return sampleDomeRadiance(worldToDome * reflected) * visibility;
  vec3 tangent = normalize(cross(abs(reflected.z) < .99 ? vec3(0, 0, 1) : vec3(1, 0, 0), reflected));
  vec3 bitangent = cross(reflected, tangent);
  vec3 result = vec3(0);
  float total = 0, a = roughness * roughness;
  float ggxSlope = a * a - 1;
  // Unit circle at the centres of eight equal azimuth sectors.
  const float outer = .9238795325, inner = .3826834324;
  const vec2 azimuth[8] = vec2[8](
      vec2(outer, inner), vec2(inner, outer), vec2(-inner, outer), vec2(-outer, inner),
      vec2(-outer, -inner), vec2(-inner, -outer), vec2(inner, -outer), vec2(outer, -inner));
  // Fixed GGX quadrature is current-frame only. This bounded eight-tap fallback
  // avoids another map-sized prefilter allocation; local probes stay preferred.
  for (uint i = 0; i < 8u; ++i) {
    float u = float(bitfieldReverse(i)) * 2.3283064365386963e-10;
    float z = sqrt((1 - u) / (1 + ggxSlope * u));
    float r = sqrt(max(0, 1 - z * z));
    vec2 radial = r * azimuth[i];
    vec3 halfVector = tangent * radial.x + bitangent * radial.y + reflected * z;
    vec3 direction = reflect(-reflected, halfVector);
    float weight = max(dot(reflected, direction), 0);
    result += sampleDomeRadiance(worldToDome * direction) * weight;
    total += weight;
  }
  return result * visibility / max(total, 1e-6);
}
