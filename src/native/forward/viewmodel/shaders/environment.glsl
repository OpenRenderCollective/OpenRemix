// Default viewmodel environment resources; private to the parallax/sky source.
#include "../../shaders/baked-storage.glsl"
layout(set = 0, binding = 13, std140) uniform ViewModelSky {
  mat4 inverseProjection, viewToDome;
  vec4 radiance, target;
} sky;
layout(set = 0, binding = 14) uniform sampler2D skyMap;
#include "../../shaders/environment-sampling.glsl"
