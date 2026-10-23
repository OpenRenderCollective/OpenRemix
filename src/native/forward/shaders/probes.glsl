// Projection metadata matches forward/probes/data.h; radiance uses HDR image
// layers. No optional cube-array or descriptor-indexing features are needed.
#ifndef PROBE_BINDING
#define PROBE_BINDING 13
#endif
#ifndef PROBE_SET
#define PROBE_SET 0
#endif
struct ReflectionProbe {
  vec4 minimum, maximum, influenceMinimum, influenceMaximum, position;
  vec4 worldToLocal[3];
};
layout(set = PROBE_SET, binding = PROBE_BINDING, std430) readonly buffer ReflectionProbes {
  uvec4 probeParameters;
  ReflectionProbe probes[64];
};
#ifndef PROBE_IMAGE_BINDING
#error Reflection consumers must bind the HDR radiance image
#endif
layout(set = PROBE_SET, binding = PROBE_IMAGE_BINDING) uniform sampler2DArray probeImages;
#include "cube.glsl"
vec3 probeImageFaceTexel(uint probe, uint level, uint size, uint face, ivec2 pixel) {
  if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, ivec2(size)))) {
    vec3 adjacent = cubeProjection(cubeDirection(face, 2 * (vec2(pixel) + .5) / float(size) - 1));
    face = uint(adjacent.z);
    pixel = clamp(ivec2((adjacent.xy * .5 + .5) * float(size)), ivec2(0), ivec2(size) - 1);
  }
  return texelFetch(probeImages, ivec3(pixel.x, int(face*size) + pixel.y, int(probe)), int(level)).rgb;
}
vec3 probeImageTexel(uint probe, uint level, uint size, uint face, ivec2 pixel) {
  bvec2 outside = bvec2(pixel.x < 0 || pixel.x >= int(size), pixel.y < 0 || pixel.y >= int(size));
  if (all(outside)) {
    ivec2 edge = clamp(pixel, ivec2(0), ivec2(size)-1);
    return (probeImageFaceTexel(probe, level, size, face, edge) +
            probeImageFaceTexel(probe, level, size, face, ivec2(pixel.x, edge.y)) +
            probeImageFaceTexel(probe, level, size, face, ivec2(edge.x, pixel.y))) / 3;
  }
  return probeImageFaceTexel(probe, level, size, face, pixel);
}
vec3 probeMip(uint probe, uint level, vec3 ray) {
  uint size = probeParameters.y >> level;
  vec3 projected = cubeProjection(ray);
  vec2 pixel = (projected.xy * .5 + .5) * float(size) - .5;
  ivec2 base = ivec2(floor(pixel));
  vec2 fraction = fract(pixel);
  vec3 result = vec3(0);
  for (uint corner = 0; corner < 4u; ++corner) {
    ivec2 upper = ivec2(corner & 1u, (corner >> 1u) & 1u);
    vec2 weight = mix(1 - fraction, fraction, vec2(upper));
    result += probeImageTexel(probe, level, size, uint(projected.z), base + upper) * weight.x * weight.y;
  }
  return result;
}
// Candidates may come from screen tiles or a small object's receiver bounds.
// Unmasked users, including water, keep the contiguous walk. Both paths
// visit probes in the same header order.
vec3 probeFeather(ReflectionProbe record, vec3 localPosition) {
  vec3 halfSize = (record.influenceMaximum.xyz - record.influenceMinimum.xyz) * .5;
  vec3 inverseWidth = vec3(record.maximum.w, record.influenceMinimum.w, record.influenceMaximum.w);
  inverseWidth = mix(1 / (halfSize * .2), inverseWidth, greaterThan(inverseWidth, vec3(0)));
  vec3 distance = min(localPosition - record.influenceMinimum.xyz, record.influenceMaximum.xyz - localPosition);
  vec3 fade = clamp(distance * inverseWidth, 0, 1);
  return fade * fade * (3 - 2 * fade);
}
float probePriorityCoverage(vec3 fade) {
  // A smaller finite limit keeps shared-face ratios defined without a visible
  // priority step at the outer boundary, even for very bright HDR captures.
  fade = max(fade, vec3(1e-12));
  return fade.x * fade.y * fade.z;
}
void compositeProbeGroup(vec3 group, float total, float coverage, float maximumCoverage,
                         inout vec3 result, inout float remaining) {
  float opacity = clamp(coverage / maximumCoverage, 0, 1);
  result += (group / total) * opacity * remaining;
  remaining *= 1 - opacity;
}
vec3 probeParallaxDirection(ReflectionProbe record, vec3 position, vec3 reflected,
                           vec3 localPosition, vec3 localRay) {
  float exitDistance = 3.402823e38;
  for (uint c = 0; c < 3u; ++c)
    if (abs(localRay[c]) > 1e-10) {
      float plane = localRay[c] > 0 ? record.maximum[c] : record.minimum[c];
      exitDistance = min(exitDistance, (plane - localPosition[c]) / localRay[c]);
    }
  vec3 corrected = position + reflected * exitDistance - record.position.xyz;
  if (exitDistance <= 0 || dot(corrected, corrected) < 1e-10) corrected = reflected;
  return corrected;
}
// Explicit receiver assignments bypass influence/priority selection, while
// retaining the selected capture's projection and roughness-filtered mips.
bool boundProbeRadiance(uint layer, vec3 position, vec3 reflected, float roughness, out vec3 result) {
  result = vec3(0);
  if (layer == 0u || layer > probeParameters.x || dot(reflected, reflected) < 1e-10) return false;
  uint probe = layer - 1u;
  ReflectionProbe record = probes[probe];
  reflected = normalize(reflected);
  vec3 localPosition, localRay;
  for (uint c = 0u; c < 3u; ++c) {
    localPosition[c] = dot(record.worldToLocal[c], vec4(position, 1));
    localRay[c] = dot(record.worldToLocal[c].xyz, reflected);
  }
  vec3 corrected = probeParallaxDirection(record, position, reflected, localPosition, localRay);
  float lod = clamp(roughness, 0, 1) * float(probeParameters.z - 1u);
  uint low = uint(lod), high = min(low + 1u, probeParameters.z - 1u);
  result = mix(probeMip(probe, low, corrected), probeMip(probe, high, corrected), lod - float(low));
  return true;
}
#ifdef PROBE_TILE_CANDIDATES
bool probeRadianceMasked(vec3 position, vec3 reflected, float roughness, uvec2 candidates, out vec3 result) {
#else
bool probeRadiance(vec3 position, vec3 reflected, float roughness, out vec3 result) {
#endif
  result = vec3(0);
  if (probeParameters.x == 0u || dot(reflected, reflected) < 1e-10)
    return false;
  reflected = normalize(reflected);
  float lod = clamp(roughness, 0, 1) * float(probeParameters.z - 1u), total = 0;
  // Upload records/layers are ordered by priority. Automatic equal-priority
  // sets retain their original single traversal and normalized blend.
  bool layered = probes[0].minimum.w != probes[probeParameters.x - 1u].minimum.w;
  float maximumCoverage = 1;
  // A tier reaching this maximum is opaque and ends traversal. The ratio also
  // cancels common floor/wall bands instead of washing out their priority.
  if (layered) {
    maximumCoverage = 0;
#ifdef PROBE_TILE_CANDIDATES
    uvec2 pending = candidates;
    while (any(notEqual(pending, uvec2(0)))) {
      uint word = pending.x != 0u ? 0u : 1u;
      uint probe = word * 32u + uint(findLSB(pending[word]));
      pending[word] &= pending[word] - 1u;
      if (probe >= probeParameters.x) break;
#else
    for (uint probe = 0; probe < probeParameters.x; ++probe) {
#endif
      ReflectionProbe record = probes[probe];
      vec3 local;
      for (uint c = 0; c < 3u; ++c) local[c] = dot(record.worldToLocal[c], vec4(position, 1));
      if (any(lessThan(local, record.influenceMinimum.xyz)) || any(greaterThan(local, record.influenceMaximum.xyz))) continue;
      maximumCoverage = max(maximumCoverage, probePriorityCoverage(probeFeather(record, local)));
    }
    if (maximumCoverage == 0) return false;
  }
  vec3 group = vec3(0);
  float priority = 3.402823e38, remaining = 1, groupCoverage = layered ? 0 : 1;
  bool found = false;
  uint low = uint(lod), high = min(low + 1u, probeParameters.z - 1u);
#ifdef PROBE_TILE_CANDIDATES
  while (any(notEqual(candidates, uvec2(0)))) {
    uint word = candidates.x != 0u ? 0u : 1u;
    uint probe = word * 32u + uint(findLSB(candidates[word]));
    candidates[word] &= candidates[word] - 1u;
    if (probe >= probeParameters.x) break;
#else
  for (uint probe = 0; probe < probeParameters.x; ++probe) {
#endif
    ReflectionProbe record = probes[probe];
    vec3 localPosition, localRay;
    for (uint c = 0u; c < 3u; ++c) {
      localPosition[c] = dot(record.worldToLocal[c], vec4(position, 1));
      localRay[c] = dot(record.worldToLocal[c].xyz, reflected);
    }
    if (any(lessThan(localPosition, record.influenceMinimum.xyz)) ||
        any(greaterThan(localPosition, record.influenceMaximum.xyz))) continue;
    if (record.minimum.w != priority && total > 0) {
      compositeProbeGroup(group, total, groupCoverage, maximumCoverage, result, remaining);
      found = true;
      group = vec3(0);
      total = 0;
      groupCoverage = layered ? 0 : 1;
      if (remaining == 0) break;
    }
    priority = record.minimum.w;
    // Intersect the reflected ray with its parallax proxy. Sampling from the
    // capture origin toward that hit corrects parallax as the receiver moves.
    vec3 corrected = probeParallaxDirection(record, position, reflected, localPosition, localRay);
    vec3 halfSize = (record.influenceMaximum.xyz - record.influenceMinimum.xyz) * .5;
    vec3 feather = probeFeather(record, localPosition);
    // Independent axis bands retain doorway fades on a shared solid floor.
    // A finite edge limit preserves normalized energy at exact wall corners.
    vec3 edgeFade = max(feather, vec3(1e-4));
    float edge = edgeFade.x * edgeFade.y * edgeFade.z;
    // Shared floor/wall faces have identical zero edge fade. Prefer nearby
    // captures there as well, with a finite core relative to the room size.
    vec3 offset = position - record.position.xyz;
    float smallestHalf = min(min(halfSize.x, halfSize.y), halfSize.z);
    float coreSquared = max(.01 * smallestHalf * smallestHalf, 1e-20);
    // Prefer captures whose recorded geometry supports their projection box.
    // Legacy/unknown metadata is neutral; normalization preserves a sole probe.
    float confidence = record.position.w > 0 ? record.position.w : 1;
    float spatialWeight = confidence / max(dot(offset, offset), coreSquared);
    float weight = spatialWeight * edge;
    vec3 radiance = mix(probeMip(probe, low, corrected), probeMip(probe, high, corrected), lod - float(low));
    group += radiance * weight;
    total += weight;
    if (layered) groupCoverage = max(groupCoverage, probePriorityCoverage(feather));
  }
  if (total > 0) {
    compositeProbeGroup(group, total, groupCoverage, maximumCoverage, result, remaining);
    found = true;
  }
  return found;
}
