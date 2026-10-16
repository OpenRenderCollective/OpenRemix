#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef LIVE_GI
#extension GL_EXT_ray_query : require
#endif
#include "bindings.glsl"
#include "material.glsl"
#ifndef SHADOW_PASS
#include "brdf.glsl"
#include "irradiance.glsl"
#include "lightmap.glsl"
#include "occlusion.glsl"
#include "local-shadows.glsl"
#include "sun.glsl"
#ifdef LIVE_GI
#include "gi.glsl"
#endif
#endif
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 vertexColor;
layout(location = 3) in vec3 worldNormal;
layout(location = 4) in vec4 surfaceLightmap;
layout(location = 5) flat in uvec4 surfaceLights;
layout(location = 6) flat in uint surfaceChart;
layout(location = 7) flat in uvec4 surfaceDirectLights;
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
bool emitterAbovePlane(Light light, vec3 p, vec3 n) {
  if (light.radiance.w <= 0)
    return dot(light.positionRadius.xyz - p, n) + light.positionRadius.w > 0;
  return max(max(dot(light.corners[0].xyz - p, n), dot(light.corners[1].xyz - p, n)),
             dot(light.corners[2].xyz - p, n)) > 0;
}
vec3 localLight(uint index, vec3 p, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                vec4 directIrradiance) {
  Light light = lights[index];
  vec3 delta = light.positionRadius.xyz - p;
  float distance2 = max(dot(delta, delta), 1e-8), distance = sqrt(distance2);
  float range = light.shaping.z;
  if (distance >= range)
    return vec3(0);
  uint cachedVisibility = staticLightVisibility(surfaceChart, uint(light.shaping.w));
  if (cachedVisibility == 1u)
    return vec3(0);
  float visibility;
  uint channel;
  bool baked =
      bakedLightVisibility(surfaceLightmap, surfaceLights, uint(light.shaping.w), visibility, channel);
  baked = baked || cachedVisibility == 2u;
  float moving = localShadowVisibility(light, p, !baked);
  uint directChannel = lightChannel(surfaceDirectLights, uint(light.shaping.w));
  bool bakedDiffuse = directChannel < 4u && directIrradiance[min(directChannel, 3u)] >= 0;
  if (moving <= 0 || (!bakedDiffuse && visibility <= 0))
    return vec3(0);
  vec3 l = delta / distance;
  float shape = 1;
  if (light.directionCone.w > -1) {
    shape = smoothstep(light.directionCone.w, min(1, light.directionCone.w + max(light.shaping.x, .001)),
                       dot(-l, light.directionCone.xyz));
    if (light.shaping.y > 0)
      shape *= pow(max(dot(-l, light.directionCone.xyz), 0), light.shaping.y);
  }
  float solidAngle;
  if (light.radiance.w > 0) {
    vec3 normal =
        cross(light.corners[1].xyz - light.corners[0].xyz, light.corners[2].xyz - light.corners[0].xyz);
    float area = .5 * length(normal);
    float cosine = dot(normal / max(2 * area, 1e-8), -l);
    if (light.radiance.w > 1)
      cosine = abs(cosine);
    solidAngle = area * max(cosine, 0) / distance2;
  } else
    solidAngle =
        2 * Pi * (1 - sqrt(max(0, 1 - min(1, light.positionRadius.w * light.positionRadius.w / distance2))));
  float fade = pow(clamp(1 - pow(distance / range, 4), 0, 1), 2);
  BrdfLobes response = brdfLobes(base, roughness, metallic, n, v, l);
  // Truncated SH can ring below the receiver plane. Clip a wholly hidden
  // emitter analytically while allowing a finite emitter to cross the horizon.
  float unitDiffuse = bakedDiffuse ? (emitterAbovePlane(light, p, n) ? directIrradiance[directChannel] : 0)
                                   : max(dot(n, l), 0) * solidAngle * shape * visibility;
  vec3 diffuse = response.diffuse * unitDiffuse;
  vec3 specular = response.specular * (solidAngle * shape * visibility);
  return (diffuse + specular) * light.radiance.rgb * (fade * moving);
}
#endif
void main() {
  Material m = materials[draw.index];
  vec4 surface = surfaceColor(m, uv, vertexColor);
  alphaTest(m, surface.a);
#ifdef LOCAL_SHADOW_PASS
  vec4 lightPosition = frame.localPositions[draw.cascade / 6u];
  gl_FragDepth = min(length(worldPosition - lightPosition.xyz) / lightPosition.w, 1);
#endif
#ifndef SHADOW_PASS
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
  n = mappedNormal(m, uv, worldPosition, n, v);
  float roughness =
      (m.maps.x & 1u) != 0u ? mapValue(texture(roughnessMap, uv), m.remap[0], m.maps.z) : m.pbr.x;
  float metallic = (m.maps.x & 2u) != 0u ? mapValue(texture(metallicMap, uv), m.remap[1], m.maps.w) : m.pbr.y;
  // Filter specular at normal-map frequencies MSAA alone cannot sample.
  roughness =
      clamp(sqrt(roughness * roughness + min(.25, dot(dFdx(n), dFdx(n)) + dot(dFdy(n), dFdy(n)))), .04, 1);
  float depth = abs((frame.view * vec4(worldPosition, 1)).z);
  vec3 ambient = frame.ambientExposure.rgb, irradiance;
  vec4 surfaceLighting = vec4(0);
  bool charted = surfaceIrradiance(surfaceLightmap, n, surfaceLighting);
#ifdef LIVE_GI
  ambient = metallic < 1 && any(greaterThan(surface.rgb, vec3(0))) ? liveDiffuse(worldPosition, n, geometric)
                                                                   : vec3(0);
#else
  if (charted)
    ambient = surfaceLighting.rgb / Pi;
  else if (bakedIrradiance(worldPosition, n, irradiance))
    ambient = irradiance / Pi;
#endif
  vec3 color = surface.rgb * (1 - metallic) * ambient;
  color += sunLight(surfaceLightmap, surfaceLighting.w, worldPosition, depth, n, v,
                    surface.rgb, roughness, metallic);
  bool directChannels =
      metallic < 1 && frame.target.z > 0u && any(notEqual(surfaceDirectLights, uvec4(0xffffffffu)));
  vec4 directIrradiance = directChannels ? surfaceDirectIrradiance(surfaceLightmap, n) : vec4(-1);
  uint base = clusterIndex(gl_FragCoord.xy, depth) * ClusterStride;
  uint count = clusters[base];
  // Overflow remains correct: this cluster evaluates the full light array.
  // It is slower, but never silently loses illumination.
  for (uint i = 0u; i < (count > ClusterCapacity ? frame.target.z : count); ++i) {
    uint index = count > ClusterCapacity ? i : clusters[base + 1u + i];
    color += localLight(index, worldPosition, n, v, surface.rgb, roughness, metallic, directIrradiance);
  }
  color += emitted;
  result = vec4(min(max(color, vec3(0)), vec3(65000)), 1);
#ifdef REFLECTION_GUIDES
  outNormalRoughness = vec4(n, roughness);
  outReflectance = vec4(mix(vec3(.04), surface.rgb, metallic), 1);
#endif
#endif
}
