#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef PREPASSED_WORLD
// Coverage and depth were written by the matching alpha-tested prepass.
// This variant must use a read-only depth pipeline: discard cannot undo an
// early depth write, including at coplanar cutout boundaries.
layout(early_fragment_tests) in;
#endif
#ifdef EARLY_OPAQUE
// The host selects this variant only for ALWAYS alpha comparison and disabled
// camera backface rejection. No discard can invalidate its early depth writes.
layout(early_fragment_tests) in;
#endif
#if defined(LIVE_GI) || defined(WATER_RAY)
#extension GL_EXT_ray_query : require
#endif
#include "bindings.glsl"
#include "material.glsl"
#ifndef SHADOW_PASS
#include "brdf.glsl"
#include "emitter.glsl"
#include "surface.glsl"
#include "reflection-guide.glsl"
#define STYLED_BAKE
#include "irradiance.glsl"
#include "style-lighting.glsl"
#include "lightmap.glsl"
#include "occlusion.glsl"
#include "local-shadows.glsl"
#include "sun.glsl"
#ifdef VIEWMODEL_PASS
#include "../viewmodel/shaders/reflections.glsl"
#endif
#ifdef LIVE_GI
#include "gi.glsl"
#endif
#endif
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 vertexColor;
layout(location = 3) in vec3 worldNormal;
layout(location = 4) centroid in vec4 surfaceLightmapInput;
layout(location = 5) flat in uvec4 surfaceLights;
layout(location = 6) flat in uint surfaceChart;
layout(location = 7) flat in uvec4 surfaceDirectLights;
layout(location = 8) flat in vec4 surfaceBounds;
layout(push_constant) uniform Draw {
  uint index;
  uint cascade;
}
draw;
#ifndef SHADOW_PASS
layout(location = 0) out vec4 result;
#ifdef REFLECTION_GUIDES
layout(location = 1) out vec4 outNormalRoughness;
layout(location = 2) out vec4 outReflectance;
#endif
#include "receiver.glsl"
#include "direct-light.glsl"
#include "receiver-lighting.glsl"
// Specialization removes diagnostic branches from ordinary world shading.
layout(constant_id = 1) const uint LightingView = 0u;
#ifndef WATER_SURFACE
#include "standard-shading.glsl"
#endif
#endif
#ifdef WATER_SURFACE
#include "../water/shaders/surface.glsl"
#else
void main() {
  Material m = materials[draw.index];
#if !defined(SHADOW_PASS) && !defined(EARLY_OPAQUE)
  // Camera visibility is independent of double-sided light transport. Walls
  // still occlude bake/GI/shadow rays when viewed from their hidden side.
  if ((m.state.w & 16u) != 0u && !gl_FrontFacing)
    discard;
#endif
#ifdef SHADOW_PASS
  // Opaque casters need depth only. Avoid fetching/composing their albedo just
  // to evaluate an alpha comparison whose result is already known.
  if (m.state.y == 0u) discard;
  if (m.state.y != 7u) alphaTest(m, surfaceColor(m, uv, vertexColor).a);
#else
  vec4 surface = surfaceColor(m, uv, vertexColor);
#ifndef EARLY_OPAQUE
  alphaTest(m, surface.a);
#endif
#endif
#ifdef LOCAL_SHADOW_PASS
  vec4 lightPosition = frame.localPositions[draw.cascade / 6u];
  gl_FragDepth = min(length(worldPosition - lightPosition.xyz) / lightPosition.w, 1);
#endif
#ifndef SHADOW_PASS
  if (LightingView == 4u) {
    result = vec4(surface.rgb, m.pbr.w > .5 ? surface.a : 1);
#ifdef REFLECTION_GUIDES
    outNormalRoughness = packReflectionGuide(vec3(0, 0, 1), 1, -1);
    outReflectance = vec4(0);
#endif
    return;
  }
  vec3 v = normalize(frame.cameraPosition.xyz - worldPosition);
  vec3 geometric = cross(dFdx(worldPosition), dFdy(worldPosition));
  Surface evaluated = evaluateStandardSurface(m, surface, uv, worldPosition, worldNormal,
                                              geometric, v, LightingView != 5u);
  ShadingReceiver receiver;
  receiver.position = worldPosition;
  receiver.geometric = geometric;
  receiver.viewDirection = v;
  receiver.screenPosition = gl_FragCoord.xy;
  receiver.depth = abs((frame.view * vec4(worldPosition, 1)).z);
  vec4 coordinate = UnmappedReceiver ? vec4(0) : surfaceLightmapInput;
  coordinate.xy = clamp(coordinate.xy, surfaceBounds.xy, surfaceBounds.zw);
  receiver.baked = BakedReceiver(coordinate, surfaceBounds, surfaceLights, surfaceDirectLights, surfaceChart);
  receiver.directChannels = (m.maps.x & 16u) == 0u;
  receiver.reflectionLayer = (m.maps.x >> 8u) & 127u;
  receiver.opacity = m.pbr.w > .5 ? evaluated.color.a : 1;
  ShadingResult shaded = shadeStandardSurface(evaluated, receiver);
  result = shaded.color;
#ifdef REFLECTION_GUIDES
  outNormalRoughness = packReflectionGuide(shaded.normalRoughness.xyz, shaded.normalRoughness.w,
                                          shaded.irradianceLuma);
  outReflectance = shaded.reflectance;
#endif
#endif
}

#endif // WATER_SURFACE
