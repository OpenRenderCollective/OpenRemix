#version 450
#extension GL_GOOGLE_include_directive : require
#include "bindings.glsl"
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texcoord;
layout(location = 2) in vec4 color;
layout(location = 3) in vec3 normal;
#ifdef SURFACE_LIGHTMAP
layout(location = 4) in vec4 lightmapCoordinate;
layout(location = 5) in uvec4 lightmapLights;
layout(location = 6) in uvec4 lightmapDirectLights;
#endif
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec2 uv;
layout(location = 2) out vec4 vertexColor;
layout(location = 3) out vec3 worldNormal;
layout(location = 4) out vec4 surfaceLightmap;
layout(location = 5) flat out uvec4 surfaceLights;
layout(location = 6) flat out uint surfaceChart;
layout(location = 7) flat out uvec4 surfaceDirectLights;
layout(push_constant) uniform Draw {
  uint index;
  uint cascade;
} draw;
void main() {
  Material m = materials[draw.index];
  vec4 world = m.model * vec4(position, 1);
  worldPosition = world.xyz;
  worldNormal = mat3(m.normal) * normal;
  uv = texcoord * m.uv.xy + m.uv.zw;
  vertexColor = color;
#ifdef SURFACE_LIGHTMAP
  surfaceLightmap = lightmapCoordinate;
  surfaceLights = lightmapLights;
  surfaceDirectLights = lightmapDirectLights;
  surfaceChart = uint(lightmapCoordinate.w) - 1u;
#else
  surfaceLightmap = vec4(0);
  surfaceLights = uvec4(0xffffffffu);
  surfaceDirectLights = uvec4(0xffffffffu);
  surfaceChart = 0xffffffffu;
#endif
#ifdef LOCAL_SHADOW_PASS
  gl_Position = frame.localShadows[draw.cascade] * world;
#elif defined(SHADOW_PASS)
  gl_Position = frame.shadows[draw.cascade] * world;
#else
  gl_Position = frame.camera * world;
#endif
}
