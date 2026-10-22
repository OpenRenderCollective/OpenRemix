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
  float priority = -3.402823e38;
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
    if (record.minimum.w < priority || any(lessThan(localPosition, record.influenceMinimum.xyz)) ||
        any(greaterThan(localPosition, record.influenceMaximum.xyz))) continue;
    if (record.minimum.w > priority) {
      priority = record.minimum.w;
      result = vec3(0);
      total = 0;
    }
    // Intersect the reflected ray with its parallax proxy. Sampling from the
    // capture origin toward that hit corrects parallax as the receiver moves.
    float exitDistance = 3.402823e38;
    for (uint c = 0; c < 3u; ++c)
      if (abs(localRay[c]) > 1e-10) {
        float plane = localRay[c] > 0 ? record.maximum[c] : record.minimum[c];
        exitDistance = min(exitDistance, (plane - localPosition[c]) / localRay[c]);
      }
    vec3 corrected = position + reflected * exitDistance - record.position.xyz;
    if (exitDistance <= 0 || dot(corrected, corrected) < 1e-10)
      corrected = reflected;
    vec3 halfSize = (record.influenceMaximum.xyz - record.influenceMinimum.xyz) * .5;
    vec3 inverseWidth = vec3(record.maximum.w, record.influenceMinimum.w, record.influenceMaximum.w);
    inverseWidth = mix(1 / (halfSize * .2), inverseWidth, greaterThan(inverseWidth, vec3(0)));
    vec3 distance = min(localPosition - record.influenceMinimum.xyz, record.influenceMaximum.xyz - localPosition);
    vec3 edgeFade = clamp(distance * inverseWidth, 0, 1);
    // Independent axis bands retain doorway fades on a shared solid floor.
    // A finite edge limit preserves normalized energy at exact wall corners.
    edgeFade = max(edgeFade * edgeFade * (3 - 2 * edgeFade), vec3(1e-4));
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
    result += radiance * weight;
    total += weight;
  }
  if (total == 0)
    return false;
  result /= total;
  return true;
}
