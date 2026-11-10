// Scene descriptors for fragment-stage environment sampling.
#include "baked-storage.glsl"
layout(set = 0, binding = 13, std140) uniform SceneReflectionSky {
  mat4 inverseProjection, viewToDome;
  vec4 radiance, target;
} sky;
layout(set = 0, binding = 14) uniform sampler2D skyMap;
#include "environment-sampling.glsl"
