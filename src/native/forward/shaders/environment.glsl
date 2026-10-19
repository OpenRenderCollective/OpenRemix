// Unweighted radiance for uncovered reflection transport. The caller applies
// remaining confidence and Fresnel once, after choosing local or sky fallback.
#define IRRADIANCE_BINDING 16
#include "irradiance.glsl"
layout(set = 0, binding = 14, std140) uniform ReflectionSky {
  mat4 inverseProjection, viewToDome;
  vec4 radiance, target;
} sky;
layout(set = 0, binding = 15) uniform sampler2D skyMap;
#include "environment-sampling.glsl"
