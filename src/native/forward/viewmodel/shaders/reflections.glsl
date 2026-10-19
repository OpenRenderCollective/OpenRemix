#define PROBE_BINDING 12
#include "../../shaders/probes.glsl"
layout(set = 0, binding = 13, std140) uniform ViewModelSky {
  mat4 inverseProjection, viewToDome;
  vec4 radiance, target;
} sky;
layout(set = 0, binding = 14) uniform sampler2D skyMap;
#include "../../shaders/environment-sampling.glsl"

vec3 viewModelReflection(vec3 position, vec3 n, vec3 v, vec3 base, float roughness, float metallic) {
  if ((frame.clusterInfo.w & 2u) == 0u) return vec3(0);
  vec3 reflected = reflect(-v, n), radiance;
  if (!probeRadiance(position + n * sky.target.z, reflected, roughness, radiance))
    radiance = environmentReflection(position, n, reflected, roughness);
  vec3 f0 = mix(vec3(.04), base, metallic);
  vec3 fresnel = f0 + (max(vec3(1 - roughness), f0) - f0) * pow(1 - max(dot(n, v), 0), 5);
  return radiance * fresnel;
}
