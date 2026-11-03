// Matches forward/bindings.h. This backend uses ordinary Vulkan descriptors;
// no ray-query or descriptor-indexing features are needed.
layout(set = 0, binding = 0, std140) uniform Frame {
  mat4 camera, view, inverseProjection;
  mat4 shadows[4];
  vec4 cameraPosition, sunDirection, sunRadiance, ambientExposure;
  vec4 depth, splits;
  uvec4 grid, target;
  mat4 localShadows[48];
  vec4 localPositions[8];
  vec4 localSettings;
  vec4 styleScales[32];
  uvec4 occlusion;
  uvec4 clusterInfo;
} frame;
struct Material {
  mat4 model, normal;
  vec4 tint, uv, pbr, emission, factor;
  vec4 remap[2];
  uvec4 state, maps;
};
layout(set = 0, binding = 3, std430) readonly buffer Materials {
  Material materials[];
};
