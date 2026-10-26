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
  uvec4 bakeRecords[]; // 16 vectors per probe, optional directory, then styled/local tables
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
uint bakeLookupWord(uint word) {
  return bakeRecords[bakeDimensions.w * 16u + word / 4u][word % 4u];
}
uint bakeAddressingMode() { return floatBitsToUint(bakeSettings.z) & 7u; }
uint bakeDirectoryRecords() {
  return floatBitsToUint(bakeSettings.z) >> 3u;
}
#ifdef STYLED_BAKE
vec3 styledProbeIrradiance(uint index, float sh[9]);
#endif
#include "probe-depth.glsl"
#include "coarse-transfer.glsl"
#include "grid-stencil.glsl"
bool bakedIrradiance(vec3 position, vec3 normal, out vec3 result) {
  if (!localProbeStencil(position, normal))
    return false;
  float sh[9] = float[9](.2820947918, .4886025119 * normal.y, .4886025119 * normal.z, .4886025119 * normal.x,
                         1.092548431 * normal.x * normal.y, 1.092548431 * normal.y * normal.z,
                         .3153915653 * (3 * normal.z * normal.z - 1), 1.092548431 * normal.x * normal.z,
                         .5462742153 * (normal.x * normal.x - normal.y * normal.y));
  result = vec3(0);
  float total = 0;
  for (uint corner = 0u; corner < localStencilCount; ++corner) {
    uint index = localStencilIndices[corner];
    float weight = localStencilWeights[corner];
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
  if (!localProbeStencil(position, normal))
    return 0;
  vec3 axes = abs(ray);
  axes /= max(axes.x + axes.y + axes.z, 1e-6);
  float result = 0, total = 0;
  for (uint corner = 0u; corner < localStencilCount; ++corner) {
    uint index = localStencilIndices[corner];
    float weight = localStencilWeights[corner];
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

// Static direct occlusion is independent of sun colour/intensity and diffuse
// irradiance. Missing coverage keeps the complete shadow-map fallback.
bool bakedSunVisibility(vec3 position, vec3 normal, out float visibility) {
  if (!localProbeStencil(position, normal)) return false;
  visibility = 0;
  float total = 0;
  for (uint corner = 0u; corner < localStencilCount; ++corner) {
    uint index = localStencilIndices[corner];
    float weight = localStencilWeights[corner];
    if (weight <= 0) continue;
    if (bakeProbeCoefficient(index, 7u).w < .5 && weight > 1e-6) return false;
    visibility += weight * bakeProbeCoefficient(index, 8u).w;
    total += weight;
  }
  if (total < 1e-4) return false;
  visibility = clamp(visibility / total, 0, 1);
  return true;
}

#include "probe-visibility.glsl"
bool bakedLocalProbeResponse(vec3 position, vec3 normal, vec3 shadingNormal, uint capturedLight,
                              bool directional, out float visibility, out float unitDiffuse) {
  unitDiffuse = -1;
  ProbeLightReference reference;
  if (!resolveProbeLight(capturedLight, reference)) return false;
  uint table = reference.table, light = reference.light;
  uint direct = reference.direct, records = reference.records;
  if (!localProbeStencil(position, normal)) return false;
  float sh[9] = float[9](.2820947918, .4886025119 * shadingNormal.y, .4886025119 * shadingNormal.z,
      .4886025119 * shadingNormal.x, 1.092548431 * shadingNormal.x * shadingNormal.y,
      1.092548431 * shadingNormal.y * shadingNormal.z, .3153915653 * (3 * shadingNormal.z * shadingNormal.z - 1),
      1.092548431 * shadingNormal.x * shadingNormal.z,
      .5462742153 * (shadingNormal.x * shadingNormal.x - shadingNormal.y * shadingNormal.y));
  float diffuse = 0;
  visibility = 0;
  float total = 0;
  for (uint corner = 0u; corner < localStencilCount; ++corner) {
    uint index = localStencilIndices[corner];
    float weight = localStencilWeights[corner];
    if (weight <= 0) continue;
    float mask;
    uint entry;
    if (!localProbeMask(table, index, light, mask, entry)) {
      if (weight > 1e-6) return false;
      continue;
    }
    visibility += weight * mask;
    if (directional && direct != 0u)
      diffuse += weight * localProbeDirect(direct, records, entry, sh);
    total += weight;
  }
  if (total < 1e-4) return false;
  visibility = clamp(visibility / total, 0, 1);
  if (directional && direct != 0u) unitDiffuse = max(diffuse / total, 0);
  return true;
}
bool bakedLocalProbeVisibility(vec3 position, vec3 normal, uint capturedLight, out float visibility) {
  float ignored;
  return bakedLocalProbeResponse(position, normal, normal, capturedLight, false, visibility, ignored);
}
