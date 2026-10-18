struct IrradianceProbe {
  vec4 irradiance[9];
  vec4 moments[6];
  vec4 statistics;
};
#ifndef IRRADIANCE_BINDING
#define IRRADIANCE_BINDING 5
#endif
layout(set = 0, binding = IRRADIANCE_BINDING, std430) readonly buffer BakedIrradiance {
  vec4 bakeOriginSpacing;
  uvec4 bakeDimensions;
  vec4 bakeSettings;
  uvec4 bakeRecords[]; // 16 vectors per probe; optional sparse style words follow
};
vec4 bakeProbeCoefficient(uint index, uint coefficient) {
  return uintBitsToFloat(bakeRecords[index * 16u + coefficient]);
}
vec4 bakeProbeMoment(uint index, uint face) {
  return uintBitsToFloat(bakeRecords[index * 16u + 9u + face]);
}
vec4 bakeProbeStatistics(uint index) {
  return uintBitsToFloat(bakeRecords[index * 16u + 15u]);
}
#ifdef STYLED_BAKE
vec3 styledProbeIrradiance(uint index, float sh[9]);
#endif
bool bakeCell(inout vec3 position, vec3 normal, out uvec3 base, out vec3 fraction) {
  if (bakeDimensions.w == 0u)
    return false;
  position += normal * bakeSettings.x;
  vec3 q = (position - bakeOriginSpacing.xyz) / bakeOriginSpacing.w;
  if (any(lessThan(q, vec3(0))) || any(greaterThan(q, vec3(bakeDimensions.xyz) - 1)))
    return false;
  base = min(uvec3(q), bakeDimensions.xyz - 2u);
  fraction = q - vec3(base);
  return true;
}
float bakeProbeWeight(vec3 position, vec3 normal, uvec3 coordinate, vec3 fraction, uvec3 upper,
                      out uint index) {
  vec3 factors = mix(1 - fraction, fraction, vec3(upper));
  float weight = factors.x * factors.y * factors.z;
  index = (coordinate.z * bakeDimensions.y + coordinate.y) * bakeDimensions.x + coordinate.x;
  if (bakeProbeStatistics(index).w < .5 || weight <= 0)
    return 0;
  vec3 delta = position - (bakeOriginSpacing.xyz + vec3(coordinate) * bakeOriginSpacing.w);
  if (bakeSettings.z > .5)
    delta -= vec3(bakeProbeCoefficient(index, 0u).w, bakeProbeCoefficient(index, 1u).w,
                  bakeProbeCoefficient(index, 2u).w);
  float distance = length(delta);
  vec3 absolute = abs(delta);
  uint axis = absolute.x >= absolute.y && absolute.x >= absolute.z ? 0u
              : absolute.y >= absolute.z                           ? 1u : 2u;
  vec4 moments = bakeProbeMoment(index, axis * 2u + (delta[axis] < 0 ? 1u : 0u));
  if (moments.w <= 0)
    return 0;
  if (distance > moments.x) {
    // Directional distance variance reduces interpolation through solid walls.
    float variance = max(bakeSettings.y, moments.y - moments.x * moments.x);
    float difference = distance - moments.x;
    weight *= pow(variance / (variance + difference * difference), 3);
  }
  float facing = distance > 1e-6 ? dot(normal, delta) / distance : -1;
  // Prefer probes on the outward side of the receiver's normal.
  return weight * pow(max(.05, .5 - .5 * facing), 2);
}
bool bakedIrradiance(vec3 position, vec3 normal, out vec3 result) {
  uvec3 base;
  vec3 fraction;
  if (!bakeCell(position, normal, base, fraction))
    return false;
  float sh[9] = float[9](.2820947918, .4886025119 * normal.y, .4886025119 * normal.z, .4886025119 * normal.x,
                         1.092548431 * normal.x * normal.y, 1.092548431 * normal.y * normal.z,
                         .3153915653 * (3 * normal.z * normal.z - 1), 1.092548431 * normal.x * normal.z,
                         .5462742153 * (normal.x * normal.x - normal.y * normal.y));
  result = vec3(0);
  float total = 0;
  for (uint corner = 0; corner < 8u; ++corner) {
    uvec3 upper = uvec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u), coordinate = base + upper;
    uint index;
    float weight = bakeProbeWeight(position, normal, coordinate, fraction, upper, index);
    if (weight <= 0)
      continue;
    for (uint j = 0; j < 9u; ++j)
      result += weight * bakeProbeCoefficient(index, j).rgb * sh[j];
#ifdef STYLED_BAKE
    result += weight * styledProbeIrradiance(index, sh);
#endif
    total += weight;
  }
  if (total < 1e-4)
    return false;
  result = max(result / total, vec3(0));
  return true;
}

// Coarse static sky occlusion for an uncovered reflection. Unknown regions fail
// closed. Reuse the diffuse probe's receiver visibility to reduce wall leakage.
float bakedSkyVisibility(vec3 position, vec3 normal, vec3 ray) {
  uvec3 base;
  vec3 fraction;
  if (!bakeCell(position, normal, base, fraction))
    return 0;
  vec3 axes = abs(ray);
  axes /= max(axes.x + axes.y + axes.z, 1e-6);
  float result = 0, total = 0;
  for (uint corner = 0; corner < 8u; ++corner) {
    uvec3 upper = uvec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
    uint index;
    float weight = bakeProbeWeight(position, normal, base + upper, fraction, upper, index);
    if (weight <= 0)
      continue;
    float visible = 0;
    for (uint axis = 0; axis < 3u; ++axis) {
      vec4 moments = bakeProbeMoment(index, axis * 2u + (ray[axis] < 0 ? 1u : 0u));
      if (moments.w > 0)
        visible += axes[axis] * pow(clamp(1 - moments.z, 0, 1), 3);
    }
    result += weight * visible;
    total += weight;
  }
  return total >= 1e-4 ? result / total : 0;
}
