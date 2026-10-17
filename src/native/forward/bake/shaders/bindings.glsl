#ifndef RAY_SCENE_SET
#define RAY_SCENE_SET 0
#endif
const float RayPi = 3.141592653589793;
struct RayVertex {
  vec4 position, normal, uv, color;
};
struct RayShading {
  mat4 model, normal;
  vec4 tint, uv, pbr, emission, factor;
  vec4 remap[2];
  uvec4 state, maps;
};
struct RayMaterial {
  RayShading shading;
  uvec4 textures, flags;
  vec4 optics;
};
struct RayLight {
  vec4 positionRadius, radiance, directionCone, shaping;
  vec4 corners[3];
};
struct RayPoint {
  vec4 position, normal;
};
struct RayResult {
  vec4 irradiance[9];
  vec4 moments[6];
  vec4 statistics;
};
layout(set = RAY_SCENE_SET, binding = 0) uniform accelerationStructureEXT rayWorld;
layout(set = RAY_SCENE_SET, binding = 1, std140) uniform RayFrameBlock {
  mat4 dome;
  vec4 sunDirection, sunRadiance, domeRadiance, trace;
  uvec4 counts, control;
}
rayFrame;
layout(set = RAY_SCENE_SET, binding = 2, std430) readonly buffer RayVerticesBuffer {
  RayVertex rayVertices[];
};
layout(set = RAY_SCENE_SET, binding = 3, std430) readonly buffer RayTrianglesBuffer {
  uvec4 rayTriangles[];
};
layout(set = RAY_SCENE_SET, binding = 4, std430) readonly buffer RayMaterialsBuffer {
  RayMaterial rayMaterials[];
};
layout(set = RAY_SCENE_SET, binding = 5, std430) readonly buffer RayLightsBuffer {
  RayLight rayLights[];
};
#ifndef RAY_FRAGMENT
layout(set = RAY_SCENE_SET, binding = 6, std430) readonly buffer RayPointsBuffer {
  RayPoint rayPoints[];
};
layout(set = RAY_SCENE_SET, binding = 7, std430) writeonly buffer RayResultsBuffer {
#ifdef COMPACT_VISIBILITY
  vec4 visibilityResults[]; // three vectors per point; mirrors VisibilityResult
#else
  RayResult rayResults[];
#endif
};
#endif
layout(set = RAY_SCENE_SET, binding = 8) uniform sampler2DArray rayAtlas;
