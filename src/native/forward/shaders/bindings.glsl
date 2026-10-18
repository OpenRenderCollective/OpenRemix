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
struct Light {
  vec4 positionRadius, radiance, directionCone, shaping;
  vec4 corners[3];
};
struct Material {
  mat4 model, normal;
  vec4 tint, uv, pbr, emission, factor;
  vec4 remap[2];
  uvec4 state, maps;
};
layout(set = 0, binding = 1, std430) readonly buffer Lights {
  Light lights[];
};
#ifdef CLUSTER_BUILD
layout(set = 0, binding = 2, std430) buffer Clusters {
  uint clusters[];
};
#else
layout(set = 0, binding = 2, std430) readonly buffer Clusters {
  uint clusters[];
};
#endif
layout(set = 0, binding = 3, std430) readonly buffer Materials {
  Material materials[];
};
layout(set = 0, binding = 4) uniform sampler2DArrayShadow sunShadow;
#define ClusterCapacity frame.clusterInfo.x
#define ClusterStride (ClusterCapacity + 1u)
#define ClusterLightCount (frame.clusterInfo.y == 0u ? frame.target.z : frame.clusterInfo.y - 1u)
// Only covered receivers may omit the fully baked tail. Cluster overflow still
// visits every relevant light; uncovered receivers retain the complete fallback.
uint receiverLightCount(uint count, bool aggregateReceiver, out bool linear) {
  bool fallback = !aggregateReceiver && frame.clusterInfo.y != 0u;
  linear = fallback || count > ClusterCapacity;
  return fallback ? frame.target.z : count > ClusterCapacity ? ClusterLightCount : count;
}
float sliceDepth(uint slice) {
  return frame.depth.x * pow(frame.depth.y / frame.depth.x, float(slice) / float(frame.grid.z));
}
uint clusterIndex(vec2 pixel, float depth) {
  uvec2 tile = min(uvec2(pixel) * frame.grid.xy / frame.target.xy, frame.grid.xy - 1u);
  uint z = uint(
      clamp(log(max(depth, frame.depth.x) / frame.depth.x) * frame.depth.z, 0, float(frame.grid.z - 1u)));
  return (z * frame.grid.y + tile.y) * frame.grid.x + tile.x;
}
