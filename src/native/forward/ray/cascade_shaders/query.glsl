#ifndef OPENREMIX_CASCADE_QUERY_GLSL
#define OPENREMIX_CASCADE_QUERY_GLSL
// Receiver side of Split Radiance Cascades: sparse trilinear interpolation of
// the cascade 0 probes' irradiance, read from the build the last frame published.
#ifndef CASCADE_SET
#define CASCADE_SET 2
#endif
#define CASCADE_TABLE_ACCESS readonly
#include "common.glsl"
layout(set = CASCADE_SET, binding = 2) uniform sampler2D cascadeIrradiance;

// RGB: weighted irradiance / pi; A: the weight of the probes that exist.
vec4 cascadeLevelIrradiance(vec3 position, vec3 normal, uint lod) {
  vec3 grid = cascadeAnchor(position, normal, lod, 0u) / cascadeSpacing(lod, 0u) - .5;
  ivec3 base = ivec3(floor(grid));
  vec3 fraction = grid - vec3(base);
  vec2 texel = 1 + (cascadeOctEncode(normal) * .5 + .5) * float(CascadeTile - 2u);
  vec4 sum = vec4(0);
  for (int corner = 0; corner < 8; ++corner) {
    ivec3 offset = ivec3(corner & 1, (corner >> 1) & 1, corner >> 2);
    vec3 axis = mix(1 - fraction, fraction, vec3(offset));
    float weight = axis.x * axis.y * axis.z;
    if (weight <= 0) continue;
    uint probe = cascadeFind(cascadeKey(base + offset, lod, 0u));
    if (probe == CascadeInvalid) continue;
    vec2 tile = vec2(probe % CascadeTileRow, probe / CascadeTileRow) * float(CascadeTile);
    vec4 value = textureLod(cascadeIrradiance, (tile + texel) / CascadeAtlasExtent, 0);
    // A tile's alpha is the part of its hemisphere that received rays.
    weight *= value.a;
    sum += vec4(value.rgb * weight, weight);
  }
  return sum;
}

// RGB: irradiance / pi; A: confidence, zero where no probe covers the receiver.
vec4 cascadeDiffuse(vec3 position, vec3 normal) {
  if (cascadeFrame.config.z == 0u) return vec4(0);
  float blend;
  uint lod = cascadeLod(position, blend);
  vec4 near = cascadeLevelIrradiance(position, normal, lod);
  vec4 far = blend > 0 ? cascadeLevelIrradiance(position, normal, lod + 1u) : vec4(0);
  // Either level stands in for the other where only one has probes.
  float nearWeight = near.a * (1 - blend), farWeight = far.a * blend;
  if (near.a > 0 && far.a <= 0) nearWeight = near.a;
  if (far.a > 0 && near.a <= 0) farWeight = far.a;
  float total = nearWeight + farWeight;
  if (total <= 1e-5) return vec4(0);
  vec3 value = (near.a > 0 ? near.rgb / near.a * nearWeight : vec3(0)) +
               (far.a > 0 ? far.rgb / far.a * farWeight : vec3(0));
  // Any covering probe replaces the fallback entirely: a partial blend with
  // flat ambient shows up as speckle wherever a probe's neighbours are missing.
  return vec4(max(value / total, vec3(0)), 1);
}
#endif
