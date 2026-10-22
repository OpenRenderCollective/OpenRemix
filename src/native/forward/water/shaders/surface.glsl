#include "../parameters.glsl"
#include "../optics.glsl"
layout(set = 2, binding = 0) uniform sampler2D savedHdr;
layout(set = 2, binding = 1) uniform sampler2D savedDepth;
struct WaterMaterial { vec4 transmittance, optical, scroll01, scroll2; };
layout(set = 2, binding = 3, std430) readonly buffer WaterMaterials { WaterMaterial waterMaterials[]; };
#define PROBE_SET 2
#define PROBE_BINDING 4
#define PROBE_IMAGE_BINDING 11
#include "../../shaders/probes.glsl"
layout(set = 2, binding = 5, std140) uniform WaterSky {
  mat4 inverseProjection, viewToDome;
  vec4 radiance, target;
} waterSky;
layout(set = 2, binding = 6) uniform sampler2D waterDome;
layout(set = 2, binding = 8) uniform sampler2D depthPyramid;
layout(set = 2, binding = 9) uniform sampler2D normals;
layout(set = 2, binding = 10) uniform sampler2D opaqueColor;
#include "../../shaders/reflection_math.glsl"
#include "../../shaders/radiance-filter.glsl"
#include "../../shaders/screen-reflection.glsl"
#ifdef WATER_RAY
#include "hardware.glsl"
#endif
#include "normal.glsl"
float waterOpaqueDepth(ivec2 pixel) { return texelFetch(savedDepth, pixel, 0).r; }
#include "../screen.glsl"
vec3 waterSkyRadiance(vec3 worldDirection) {
  if (waterSky.radiance.w == 0) return waterSky.radiance.rgb;
  vec3 d = normalize(mat3(waterSky.viewToDome) * mat3(frame.view) * worldDirection);
  vec2 coordinate = vec2(atan(d.z, d.x) / (2 * Pi) + .5, acos(clamp(d.y, -1, 1)) / Pi);
  return textureLod(waterDome, coordinate, 0).rgb * waterSky.radiance.rgb;
}
void main() {
  Material m = materials[draw.index];
  WaterMaterial water = waterMaterials[draw.cascade];
  vec4 surface = surfaceColor(m, uv, vertexColor);
  alphaTest(m, surface.a);
  vec3 emitted;
  float reflectedScale;
  emissionLobes(m.emission, m.state, surface, texture(albedoMap, uv),
      (m.maps.x & 8u) != 0u ? texture(emissionMap, uv) : vec4(1), emitted, reflectedScale);
  surface.rgb *= reflectedScale;
  vec3 v = normalize(frame.cameraPosition.xyz - worldPosition);
  vec3 geometric = cross(dFdx(worldPosition), dFdy(worldPosition));
  vec3 n = normalize(dot(worldNormal, worldNormal) > 1e-10 ? worldNormal : geometric);
  if (dot(n, v) < 0) n = -n;
  n = waterNormal(m, water, uv, worldPosition, n, v);
  float roughness = (m.maps.x & 1u) != 0u ? mapValue(texture(roughnessMap, uv), m.remap[0], m.maps.z) : m.pbr.x;
  roughness = clamp(sqrt(roughness * roughness + min(.25, dot(dFdx(n), dFdx(n)) + dot(dFdy(n), dFdy(n)))), .04, 1);
  float index = water.optical.x;
  bool thinSheet = water.optical.w > 0;
  WaterBoundary boundary = waterBoundary(-v, n, index, waterFrame.settings.y > .5);
  vec3 p = (frame.view * vec4(worldPosition, 1)).xyz;
  float depth = abs(p.z);
  if (thinSheet) {
    DielectricSheetPath path = dielectricSheetPath(-v, n, water.optical.w, 1, index);
    boundary = WaterBoundary(-v, path.reflected, 0, path.totalInternal);
    p += mat3(frame.view) * path.exitOffset;
  }
  ivec2 extent = ivec2(waterFrame.target.xy);
  WaterScreenHit behind = WaterScreenHit(vec2(0), 0, false);
  if (!boundary.totalInternal)
    behind = traceWaterRefraction(p, mat3(frame.view) * boundary.refracted,
        waterFrame.projection, frame.inverseProjection, extent, frame.depth.w,
        waterFrame.target.z, waterFrame.target.w, waterFrame.settings.x);
  // A known straight-through opaque layer is a conservative fallback when the
  // bent screen ray leaves the available depth layer. Foreground is never used.
  if (!behind.valid && !boundary.totalInternal) {
    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), extent - 1);
    float depth = waterOpaqueDepth(pixel);
    vec2 coordinate = (vec2(pixel) + .5) / vec2(extent);
    vec4 point = frame.inverseProjection * vec4(coordinate * 2 - 1, depth, 1);
    if (depth < 1 && abs(point.w) > 1e-8 &&
        (point.z / point.w - p.z) * frame.depth.w >= -waterFrame.target.w)
      behind = WaterScreenHit(coordinate, length(point.xyz / point.w - p), true);
  }
  vec3 background = vec3(0);
  if (behind.valid) {
    vec4 sampleColor = textureLod(savedHdr, behind.uv, 0);
    background = sampleColor.rgb / max(sampleColor.a, 1e-6);
  } else if (thinSheet && !boundary.totalInternal) {
    // A parallel pane leaves a distant sky direction unchanged. Unlike a
    // body, it has a known finite absorption distance even without opaque depth.
    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), extent - 1);
    // The visible sky is composed later, so it is absent from saved opaque HDR.
    // Uniform camera environment lighting alone does not enable a background.
    if (waterOpaqueDepth(pixel) >= 1 && waterSky.radiance.w != 0)
      background = waterSkyRadiance(-v);
  }
  float surfaceWeight = 0;
  vec3 through;
  float f0 = pow((index - 1) / (index + 1), 2);
  vec3 reflectedWeight = vec3(boundary.reflectance), specularF0 = vec3(f0);
  if (thinSheet) {
    vec3 tintDepth = -log(clamp(texture(albedoMap, uv).rgb, vec3(1e-6), vec3(1))) * water.transmittance.w;
    vec3 depth = min(water.transmittance.rgb + tintDepth, vec3(80));
    DielectricSheet sheet = dielectricSheet(depth, 1, abs(dot(n, v)), 1, index);
    through = sheet.transmission;
    reflectedWeight = sheet.reflection;
    specularF0 = dielectricSheet(depth, 1, 1, 1, index).reflection;
    // Absorption is lost energy, not diffuse body lighting.
  } else
    through = waterThrough(boundary, water.transmittance.rgb, water.transmittance.w,
        behind.distance, behind.valid, waterFrame.settings.y > .5, surfaceWeight);
  vec3 irradiance, diffuse = surface.rgb * frame.ambientExposure.rgb;
  bool aggregateReceiver = bakedIrradiance(worldPosition, n, irradiance);
  if (aggregateReceiver) diffuse = surface.rgb * irradiance / Pi;
  vec3 l = -frame.sunDirection.xyz;
  BrdfLobes sun = brdfLobes(surface.rgb, roughness, 0, n, v, l, specularF0);
  float visibility = sunShadowVisibility(worldPosition, depth, true);
  diffuse += sun.diffuse * max(dot(n, l), 0) * frame.sunRadiance.rgb * visibility;
  vec3 specular = sun.specular * frame.sunRadiance.rgb * visibility;
  uint base = clusterIndex(gl_FragCoord.xy, depth) * ClusterStride;
  bool linear;
  uint lightCount = receiverLightCount(base, aggregateReceiver, linear);
  for (uint i = 0u; i < lightCount; ++i) {
    uint light = linear ? i : clusters[base + ClusterHeaderWords + i];
    BrdfLobes response = localLightResponse(light, worldPosition, n, v, surface.rgb, roughness, 0,
                                           vec4(-1), vec4(0), specularF0, normalize(geometric), aggregateReceiver);
    diffuse += response.diffuse;
    specular += response.specular;
  }
  vec3 reflected;
  if (!probeRadiance(worldPosition, boundary.reflected, roughness, reflected))
    reflected = waterSkyRadiance(boundary.reflected) *
                bakedSkyVisibility(worldPosition, n, boundary.reflected);
  ScreenReflectionView screen = ScreenReflectionView(frame.camera, waterFrame.inverseCamera, frame.view,
      waterFrame.reflectionTrace, waterFrame.reflectionConfig, uvec2(extent));
  vec4 screenHit = traceScreenReflection(screen, worldPosition, n, boundary.reflected, -v,
                                         roughness, gl_FragCoord.xy / vec2(extent));
#ifdef WATER_RAY
  screenHit = waterHardwareReflection(worldPosition, n, boundary.reflected, roughness, screenHit);
#endif
  reflected = screenHit.rgb + (1 - screenHit.a) * reflected;
  // Bodies retain lit absorption colour; finite sheets lose absorbed energy.
  // Both use the shared filtered reflection path and their optical weights.
  vec3 color = through * background + max(0, surfaceWeight - boundary.reflectance) * diffuse +
               reflectedWeight * reflected + specular + (thinSheet ? 1 : surfaceWeight) * emitted;
  result = vec4(clamp(color, vec3(0), vec3(65000)), 1);
}
