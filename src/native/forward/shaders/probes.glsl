// Packed cubemaps use a storage buffer, avoiding optional cube-array and
// descriptor-indexing features. Layout matches forward/probes/data.h.
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
  ReflectionProbe probes[32];
  vec4 probeTexels[];
};
#include "cube.glsl"
vec3 probeTexel(uint start, uint size, uint face, ivec2 pixel) {
  if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, ivec2(size)))) {
    vec3 adjacent = cubeProjection(cubeDirection(face, 2 * (vec2(pixel) + .5) / float(size) - 1));
    face = uint(adjacent.z);
    pixel = clamp(ivec2((adjacent.xy * .5 + .5) * float(size)), ivec2(0), ivec2(size) - 1);
  }
  return probeTexels[start + (face * size + uint(pixel.y)) * size + uint(pixel.x)].rgb;
}
vec3 probeMip(uint probe, uint level, vec3 ray) {
  uint start = probe * probeParameters.w;
  for (uint i = 0; i < level; ++i) {
    uint size = probeParameters.y >> i;
    start += 6u * size * size;
  }
  uint size = probeParameters.y >> level;
  vec3 projected = cubeProjection(ray);
  vec2 pixel = (projected.xy * .5 + .5) * float(size) - .5;
  ivec2 base = ivec2(floor(pixel));
  vec2 fraction = fract(pixel);
  vec3 result = vec3(0);
  for (uint corner = 0; corner < 4u; ++corner) {
    ivec2 upper = ivec2(corner & 1u, (corner >> 1u) & 1u);
    vec2 weight = mix(1 - fraction, fraction, vec2(upper));
    result += probeTexel(start, size, uint(projected.z), base + upper) * weight.x * weight.y;
  }
  return result;
}
bool probeRadiance(vec3 position, vec3 reflected, float roughness, out vec3 result) {
  result = vec3(0);
  if (probeParameters.x == 0u || dot(reflected, reflected) < 1e-10)
    return false;
  reflected = normalize(reflected);
  float lod = clamp(roughness, 0, 1) * float(probeParameters.z - 1u), total = 0;
  float priority = -3.402823e38;
  uint low = uint(lod), high = min(low + 1u, probeParameters.z - 1u);
  for (uint probe = 0; probe < probeParameters.x; ++probe) {
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
    vec3 center = (record.influenceMinimum.xyz + record.influenceMaximum.xyz) * .5;
    vec3 halfSize = (record.influenceMaximum.xyz - record.influenceMinimum.xyz) * .5;
    vec3 distance = abs((localPosition - center) / halfSize);
    float edge = 1 - max(max(distance.x, distance.y), distance.z);
    // Shared floor/wall faces have identical zero edge fade. Prefer nearby
    // captures there as well, with a finite core relative to the room size.
    vec3 offset = position - record.position.xyz;
    float smallestHalf = min(min(halfSize.x, halfSize.y), halfSize.z);
    float coreSquared = max(.01 * smallestHalf * smallestHalf, 1e-20);
    // Prefer captures whose recorded geometry supports their projection box.
    // Legacy/unknown metadata is neutral; normalization preserves a sole probe.
    float confidence = record.position.w > 0 ? record.position.w : 1;
    float weight = confidence * pow(max(.001, edge), 2) / max(dot(offset, offset), coreSquared);
    result +=
        mix(probeMip(probe, low, corrected), probeMip(probe, high, corrected), lod - float(low)) * weight;
    total += weight;
  }
  if (total == 0)
    return false;
  result /= total;
  return true;
}
