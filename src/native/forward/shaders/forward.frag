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
#include "direct-light.glsl"
// Specialization removes diagnostic branches from ordinary world shading.
layout(constant_id = 1) const uint LightingView = 0u;
vec3 directLightComponent(BrdfLobes light, uint view) {
  if (view == 2u || view == 7u) return light.diffuse;
  if (view == 3u) return light.specular;
  return light.diffuse + light.specular;
}
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
    outNormalRoughness = vec4(0, 0, 1, 1);
    outReflectance = vec4(0);
#endif
    return;
  }
  vec4 emissionTexel = (m.maps.x & 8u) != 0u ? texture(emissionMap, uv) : vec4(1);
  vec3 emitted;
  float reflected;
  emissionLobes(m.emission, m.state, surface, texture(albedoMap, uv), emissionTexel, emitted, reflected);
  surface.rgb *= reflected;
  vec3 v = normalize(frame.cameraPosition.xyz - worldPosition);
  vec3 geometric = cross(dFdx(worldPosition), dFdy(worldPosition));
  vec3 n = dot(worldNormal, worldNormal) > 1e-10 ? normalize(worldNormal) : normalize(geometric);
  if (dot(n, v) < 0)
    n = -n;
  if (LightingView != 5u) n = mappedNormal(m, uv, worldPosition, n, v);
  float roughness =
      (m.maps.x & 1u) != 0u ? mapValue(texture(roughnessMap, uv), m.remap[0], m.maps.z) : m.pbr.x;
  float metallic = (m.maps.x & 2u) != 0u ? mapValue(texture(metallicMap, uv), m.remap[1], m.maps.w) : m.pbr.y;
  // Filter specular at normal-map frequencies MSAA alone cannot sample.
  roughness =
      clamp(sqrt(roughness * roughness + min(.25, dot(dFdx(n), dFdx(n)) + dot(dFdy(n), dFdy(n)))), .04, 1);
  float depth = abs((frame.view * vec4(worldPosition, 1)).z);
  vec3 ambient = frame.ambientExposure.rgb, irradiance;
  // Explicit subrectangle bounds isolate chart neighbours, including MSAA edge
  // fragments. Outward-rounded grids already contain every required mip-zero
  // bilinear tap, so no extra atlas border is needed.
  vec4 surfaceLightmap = UnmappedReceiver ? vec4(0) : surfaceLightmapInput;
  surfaceLightmap.xy = clamp(surfaceLightmap.xy, surfaceBounds.xy, surfaceBounds.zw);
  vec4 surfaceLighting = vec4(0);
  bool charted = surfaceIrradiance(surfaceLightmap, n, surfaceLighting);
  uint debugView = LightingView;
  if (debugView == 6u) {
    bool covered = charted || localProbeStencil(worldPosition, n);
    result = vec4(charted ? vec3(0, 0, 1) : covered ? vec3(0, 1, 0) : vec3(1, 0, 0),
                  m.pbr.w > .5 ? surface.a : 1);
#ifdef REFLECTION_GUIDES
    outNormalRoughness = vec4(n, roughness);
    outReflectance = vec4(0);
#endif
    return;
  }
  if (debugView == 1u || debugView == 5u) {
    // Neutral irradiance: exclude albedo, metallic, emission and reflections.
    vec3 indirect = charted ? surfaceLighting.rgb / Pi
                           : bakedIrradiance(worldPosition, n, irradiance) ? irradiance / Pi : ambient;
    result = vec4(min(max(indirect, vec3(0)), vec3(65000)), m.pbr.w > .5 ? surface.a : 1);
#ifdef REFLECTION_GUIDES
    outNormalRoughness = vec4(n, roughness);
    outReflectance = vec4(0);
#endif
    return;
  }
  bool aggregateReceiver = false;
#ifdef LIVE_GI
  ambient = metallic < 1 && any(greaterThan(surface.rgb, vec3(0))) ? liveDiffuse(worldPosition, n, geometric)
                                                                   : vec3(0);
#else
  if (charted) {
    ambient = surfaceLighting.rgb / Pi;
    aggregateReceiver = true;
#ifdef ZERO_DIFFUSE_MATERIAL
  } else if (localProbeStencil(worldPosition, n)) {
    ambient = vec3(0);
#else
  } else if (bakedIrradiance(worldPosition, n, irradiance)) {
    ambient = irradiance / Pi;
#endif
    aggregateReceiver = true;
  }
#endif
  bool includeBakedDiffuse = debugView == 0u || debugView == 7u;
  vec3 color = includeBakedDiffuse ? surface.rgb * (1 - metallic) * ambient : vec3(0);
  BrdfLobes sun = sunLightResponse(surfaceLightmap, surfaceLighting.w, worldPosition, depth, n, v,
                                   surface.rgb, roughness, metallic);
  color += directLightComponent(sun, debugView);
  bool directChannels =
      (m.maps.x & 16u) == 0u && !indexedSurfaceDirect() && metallic < 1 && frame.target.z > 0u &&
      any(notEqual(surfaceDirectLights, uvec4(0xffffffffu)));
  vec4 directIrradiance = directChannels ? surfaceDirectIrradiance(surfaceLightmap, n) : vec4(-1);
  uint base = clusterIndex(gl_FragCoord.xy, depth) * ClusterStride;
  // Overflow remains correct: this cluster evaluates the full light array.
  // It is slower, but never silently loses illumination.
  bool linear;
  uint lightCount = receiverLightCount(base, aggregateReceiver, linear);
  for (uint i = 0u; i < lightCount; ++i) {
    uint index = linear ? i : clusters[base + ClusterHeaderWords + i];
    BrdfLobes local = localLightResponse(index, worldPosition, n, v, surface.rgb, roughness, metallic,
                                         directIrradiance, surfaceLightmap, vec3(.04), normalize(geometric),
                                         aggregateReceiver);
    color += directLightComponent(local, debugView);
  }
  if (debugView == 0u) color += emitted;
#ifdef VIEWMODEL_PASS
  if (debugView == 0u) color += viewModelReflection(worldPosition, n, v, surface.rgb, roughness, metallic,
                                                  (m.maps.x >> 8u) & 127u);
#endif
  result = vec4(min(max(color, vec3(0)), vec3(65000)), m.pbr.w > .5 ? surface.a : 1);
#ifdef REFLECTION_GUIDES
  outNormalRoughness = vec4(n, roughness);
  // The guide is RGBA8 UNORM. Reserve its upper 65 alpha codes for ordinary
  // selection and one-based layer IDs, from this same covered MSAA sample.
  outReflectance = vec4(mix(vec3(.04), surface.rgb, metallic),
                        float(255u - ((m.maps.x >> 8u) & 127u)) / 255);
#endif
#endif
}

#endif // WATER_SURFACE
