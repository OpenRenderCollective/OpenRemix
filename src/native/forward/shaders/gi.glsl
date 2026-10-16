// Reuse the material-aware radiance transport without a second lighting model.
// This descriptor set is present only in optional hardware-GI pipelines.
#define RAY_SCENE_SET 2
#define RAY_FRAGMENT 1
#include "../bake/shaders/bindings.glsl"
#include "../bake/shaders/material.glsl"
#include "../bake/shaders/radiance.glsl"

vec3 liveDiffuse(vec3 position, vec3 normal, vec3 geometric) {
  geometric = dot(geometric, geometric) > 1e-12 ? normalize(geometric) : normal;
  if (dot(geometric, normal) < 0)
    geometric = -geometric;
  vec3 origin = position + geometric * rayFrame.trace.y;
  uint seed = floatBitsToUint(position.x) * 73856093u ^ floatBitsToUint(position.y) * 19349663u ^
              floatBitsToUint(position.z) * 83492791u;
  Medium medium = initialMedium(origin, seed);
  float rotation = random(seed) * 2 * RayPi;
  vec3 sum = vec3(0);
  for (uint i = 0; i < rayFrame.counts.z; ++i) {
    float phase = float(i * 0x61c88647u) * (1.0 / 4294967296.0);
    vec3 direction = basisDirection(normal, sqrt((float(i) + .5) / float(rayFrame.counts.z)),
                                    rotation + 2 * RayPi * phase);
    float distance;
    bool back, miss;
    sum += radiance(origin, direction, medium, seed, distance, back, miss);
  }
  // Cosine-weighted sampling cancels the diffuse cosine/Pi factor. Returning
  // E/Pi matches the baked ambient term and replaces it, including valid black.
  return max(sum / float(rayFrame.counts.z), vec3(0));
}
