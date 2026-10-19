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

// Static direct occlusion is independent of sun colour/intensity and diffuse
// irradiance. Missing coverage keeps the complete shadow-map fallback.
bool bakedSunVisibility(vec3 position, vec3 normal, out float visibility) {
  uvec3 base;
  vec3 fraction;
  if (!bakeCell(position, normal, base, fraction)) return false;
  visibility = 0;
  float total = 0;
  for (uint corner = 0u; corner < 8u; ++corner) {
    uvec3 upper = uvec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
    uint index;
    float weight = bakeProbeWeight(position, normal, base + upper, fraction, upper, index);
    if (weight <= 0) continue;
    if (bakeProbeCoefficient(index, 7u).w < .5 && weight > 1e-6) return false;
    visibility += weight * bakeProbeCoefficient(index, 8u).w;
    total += weight;
  }
  if (total < 1e-4) return false;
  visibility = clamp(visibility / total, 0, 1);
  return true;
}

uint localProbeWord(uint base, uint word) {
  return bakeRecords[base + word / 4u][word % 4u];
}
// Private to one shader invocation. Local lights at the same receiver share
// interpolation work; ray/reflection queries at another point must rebuild it.
bool localStencilInitialized = false;
bool localStencilValid = false;
vec3 localStencilPosition, localStencilNormal;
uint localStencilIndices[8];
float localStencilWeights[8];
bool localProbeStencil(vec3 position, vec3 normal) {
  if (localStencilInitialized && all(equal(position, localStencilPosition)) &&
      all(equal(normal, localStencilNormal))) return localStencilValid;
  localStencilInitialized = true;
  localStencilValid = false;
  localStencilPosition = position;
  localStencilNormal = normal;
  uvec3 base;
  vec3 fraction;
  if (!bakeCell(position, normal, base, fraction)) return false;
  float total = 0;
  for (uint corner = 0u; corner < 8u; ++corner) {
    uvec3 upper = uvec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
    uint index;
    float weight = bakeProbeWeight(position, normal, base + upper, fraction, upper, index);
    localStencilIndices[corner] = index;
    localStencilWeights[corner] = weight;
    total += weight;
  }
  localStencilValid = total >= 1e-4;
  return localStencilValid;
}
bool localProbeMask(uint base, uint probe, uint light, out float visibility, out uint entry) {
  entry = 0u;
  if (localProbeWord(base, 1u) == 3u) {
    uint count = localProbeWord(base, 3u), rowWords = localProbeWord(base, 4u + count);
    uint rows = 5u + count, row = rows + (probe * rowWords + light / 32u) * 2u;
    uint bits = localProbeWord(base, row), bit = light % 32u;
    if ((bits & (1u << bit)) == 0u) return false;
    uint rank = localProbeWord(base, row + 1u) + uint(bitCount(bits & ((1u << bit) - 1u)));
    uint offsets = rows + bakeDimensions.w * rowWords * 2u;
    uint index = localProbeWord(base, offsets + probe) + rank;
    entry = index;
    visibility = uintBitsToFloat(localProbeWord(base, offsets + bakeDimensions.w + 1u + index));
    return true;
  }
  uint offsets = 4u + localProbeWord(base, 3u);
  uint low = localProbeWord(base, offsets + probe), end = localProbeWord(base, offsets + 1u + probe), high = end;
  uint entries = offsets + 1u + bakeDimensions.w;
  while (low < high) {
    uint middle = low + (high - low) / 2u;
    if (localProbeWord(base, entries + middle * 2u) < light) low = middle + 1u;
    else high = middle;
  }
  if (low >= end || localProbeWord(base, entries + low * 2u) != light) return false;
  entry = low;
  visibility = uintBitsToFloat(localProbeWord(base, entries + low * 2u + 1u));
  return true;
}
float localProbeDirect(uint indices, uint records, uint entry, float sh[9]) {
  uint field = localProbeWord(0u, indices + entry);
  if (field == 0u) return 0;
  uint record = records + (field - 1u) * 6u;
  float value = 0;
  for (uint pair = 0u; pair < 4u; ++pair)
    value += dot(unpackHalf2x16(localProbeWord(0u, record + pair)),
                 vec2(sh[pair * 2u], sh[pair * 2u + 1u]));
  value += unpackHalf2x16(localProbeWord(0u, record + 4u)).x * sh[8];
  return value * uintBitsToFloat(localProbeWord(0u, record + 5u));
}
bool bakedLocalProbeResponse(vec3 position, vec3 normal, vec3 shadingNormal, uint capturedLight,
                              bool directional, out float visibility, out float unitDiffuse) {
  unitDiffuse = -1;
  uint table = floatBitsToUint(bakeSettings.w) / 2u;
  if (capturedLight == 0u || table == 0u || localProbeWord(table, 0u) != 0x56504c52u ||
      localProbeWord(table, 2u) != bakeDimensions.w) return false;
  uint direct = 0u;
  uint records = 0u;
  if (localProbeWord(table, 1u) == 7u) {
    direct = (table + 2u) * 4u + localProbeWord(table, 3u);
    records = direct + localProbeWord(table, 4u);
    table += 2u; // wrapper and scalar payload are both vec4 aligned
  }
  uint light = capturedLight - 1u, version = localProbeWord(table, 1u), count = localProbeWord(table, 3u);
  if (version == 3u) {
    if (light >= count) return false;
    light = localProbeWord(table, 4u + light);
    if (light == 0xffffffffu) return false;
  } else if (version == 2u) {
    if (light / 32u >= count ||
        (localProbeWord(table, 4u + light / 32u) & (1u << (light % 32u))) == 0u) return false;
  } else return false;
  if (!localProbeStencil(position, normal)) return false;
  float sh[9] = float[9](.2820947918, .4886025119 * shadingNormal.y, .4886025119 * shadingNormal.z,
      .4886025119 * shadingNormal.x, 1.092548431 * shadingNormal.x * shadingNormal.y,
      1.092548431 * shadingNormal.y * shadingNormal.z, .3153915653 * (3 * shadingNormal.z * shadingNormal.z - 1),
      1.092548431 * shadingNormal.x * shadingNormal.z,
      .5462742153 * (shadingNormal.x * shadingNormal.x - shadingNormal.y * shadingNormal.y));
  float diffuse = 0;
  visibility = 0;
  float total = 0;
  for (uint corner = 0u; corner < 8u; ++corner) {
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
