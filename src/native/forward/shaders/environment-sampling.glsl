// Shared local-to-dome sampling and visibility-filtered sky fallback.
vec3 skyRadiance(vec3 direction) {
  vec3 local = normalize(mat3(sky.viewToDome) * mat3(frame.view) * direction);
  vec2 uv = vec2(atan(local.z, local.x) / 6.28318530718 + .5,
                 acos(clamp(local.y, -1, 1)) / 3.14159265359);
  return textureLod(skyMap, uv, 0).rgb * sky.radiance.rgb;
}
vec3 environmentReflection(vec3 position, vec3 normal, vec3 reflected, float roughness) {
  if (sky.radiance.w == 0)
    return vec3(0);
  float visibility = bakedSkyVisibility(position, normal, reflected);
  if (visibility <= 0)
    return vec3(0);
  if (roughness < .025)
    return skyRadiance(reflected) * visibility;
  vec3 tangent = normalize(cross(abs(reflected.z) < .99 ? vec3(0, 0, 1) : vec3(1, 0, 0), reflected));
  vec3 bitangent = cross(reflected, tangent);
  vec3 result = vec3(0);
  float total = 0, a = roughness * roughness;
  // Fixed GGX quadrature is current-frame only. This bounded eight-tap fallback
  // avoids another map-sized prefilter allocation; local probes stay preferred.
  for (uint i = 0; i < 8u; ++i) {
    float u = float(bitfieldReverse(i)) * 2.3283064365386963e-10;
    float phi = 6.28318530718 * (float(i) + .5) / 8;
    float z = sqrt((1 - u) / (1 + (a * a - 1) * u));
    float r = sqrt(max(0, 1 - z * z));
    vec3 halfVector = tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + reflected * z;
    vec3 direction = reflect(-reflected, halfVector);
    float weight = max(dot(reflected, direction), 0);
    result += skyRadiance(direction) * weight;
    total += weight;
  }
  return result * visibility / max(total, 1e-6);
}
