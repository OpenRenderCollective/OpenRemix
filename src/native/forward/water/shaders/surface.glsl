#include "../../shaders/standard-local-lighting.glsl"
#include "../parameters.glsl"
#include "../optics.glsl"
layout(set = 2, binding = 0) uniform sampler2D savedHdr;
layout(set = 2, binding = 1) uniform sampler2D savedDepth;
struct WaterMaterial { vec4 transmittance, optical, scroll01, scroll2; uvec4 probeCandidates; };
layout(set = 2, binding = 3, std430) readonly buffer WaterMaterials { WaterMaterial waterMaterials[]; };
#define PROBE_SET 2
#define PROBE_BINDING 4
#define PROBE_IMAGE_BINDING 11
#define MASKED_REFLECTION_PROBES
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
  return decodeColorTexel(textureLod(waterDome, coordinate, 0), waterSky.target.w > .5).rgb * waterSky.radiance.rgb;
}
#include "../../shaders/reflection-lighting.glsl"
LightContribution sampleWaterLighting(ShadingReceiver receiver, vec3 n, vec3 base, float roughness,
                                      vec3 specularF0, float bodyWeight, out LightingReference reference) {
#ifdef LIVE_GI
  // A zero body weight discards incoming diffuse, including on thin sheets.
  // Still ask the provider for attribution and reflection metadata.
  bool diffuseRequired = bodyWeight > 0;
#else
  const bool diffuseRequired = true;
#endif
  ReceiverLighting lighting = sampleDiffuseLighting(receiver, n, prepareVolumeReceiverLighting(receiver, n),
                                                      diffuseRequired);
  vec3 diffuse = composeDiffuseLight(lighting, base);
  vec3 v = receiver.viewDirection;
  SunLightSample sunlight = sampleOpticalReceiverSunLight(receiver, n);
  BrdfLobes sun = brdfLobes(base, roughness, 0, n, v, sunlight.direction, specularF0);
  LightContribution sunContribution = selectDirectLight(composeSunLight(sunlight, sun), lighting.direct);
  diffuse += sunContribution.diffuse;
  vec3 specular = sunContribution.specular;
  LocalLightingContext localLighting = prepareLocalLighting(receiver, n, false, lighting.direct.attribution);
  for (uint i = 0u; i < receiverLocalLightCount(localLighting); ++i) {
    LightContribution response = standardReceiverLocalLight(i, receiver, n, v, base, roughness, 0,
                                                            localLighting, specularF0);
    response = selectDirectLight(response, lighting.direct);
    diffuse += response.diffuse;
    specular += response.specular;
  }
  reference = diffuseLightingReference(lighting, diffuseReflectionReferenceDomain());
  return LightContribution(diffuse, specular);
}
void main() {
  Material m = materials[draw.index];
  WaterMaterial water = waterMaterials[draw.cascade];
  vec4 albedo = texture(albedoMap, uv);
  vec4 surface = surfaceColor(m, albedo, vertexColor);
  alphaTest(m, surface.a);
  vec3 emitted;
  float reflectedScale;
  emissionLobes(m.emission, m.state, surface, albedo,
      (m.maps.x & 8u) != 0u ? texture(emissionMap, uv) : vec4(1), emitted, reflectedScale);
  surface.rgb *= reflectedScale;
  vec3 v = normalize(frame.cameraPosition.xyz - worldPosition);
  vec3 positionDx = dFdx(worldPosition), positionDy = dFdy(worldPosition);
  vec3 geometric = cross(positionDx, positionDy);
  vec3 n = normalize(dot(worldNormal, worldNormal) > 1e-10 ? worldNormal : geometric);
  if (dot(n, v) < 0) n = -n;
  n = waterNormal(m, water, uv, positionDx, positionDy, n, v);
  float roughness = (m.maps.x & 1u) != 0u ? mapValue(texture(roughnessMap, uv), m.remap[0], m.maps.z) : m.pbr.x;
  roughness = clamp(sqrt(roughness * roughness + min(.25, dot(dFdx(n), dFdx(n)) + dot(dFdy(n), dFdy(n)))), .04, 1);
  float index = water.optical.x;
  bool thinSheet = water.optical.w > 0;
  bool cameraInsideWater = waterFrame.settings.y > .5;
  WaterBoundary boundary;
  vec3 p = (frame.view * vec4(worldPosition, 1)).xyz;
  float depth = abs(p.z);
  if (thinSheet) {
    DielectricSheetPath path = dielectricSheetPath(-v, n, water.optical.w, 1, index);
    boundary = WaterBoundary(-v, path.reflected, 0, path.totalInternal);
    p += mat3(frame.view) * path.exitOffset;
  } else {
    boundary = waterBoundary(-v, n, index, cameraInsideWater);
  }
  ivec2 extent = ivec2(waterFrame.target.xy);
  WaterScreenHit behind = WaterScreenHit(vec2(0), 0, false);
  if (!boundary.totalInternal)
    behind = traceWaterRefraction(p, mat3(frame.view) * boundary.refracted,
        waterFrame.projection, frame.inverseProjection, extent, frame.depth.w,
        waterFrame.target.z, waterFrame.target.w, waterFrame.settings.x);
  vec3 background = vec3(0);
  // A known straight-through opaque layer is a conservative fallback when the
  // bent screen ray leaves the available depth layer. Foreground is never used.
  if (!behind.valid && !boundary.totalInternal) {
    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), extent - 1);
    float depth = waterOpaqueDepth(pixel);
    if (depth < 1) {
      vec2 coordinate = (vec2(pixel) + .5) / vec2(extent);
      vec4 point = frame.inverseProjection * vec4(coordinate * 2 - 1, depth, 1);
      if (abs(point.w) > 1e-8 && (point.z / point.w - p.z) * frame.depth.w >= -waterFrame.target.w)
        behind = WaterScreenHit(coordinate, length(point.xyz / point.w - p), true);
    } else if (depth >= 1 && thinSheet && waterSky.radiance.w != 0) {
      // A parallel pane preserves the distant direction. The dome is absent
      // from saved opaque HDR, and a uniform environment is lighting only.
      background = waterSkyRadiance(-v);
    }
  }
  if (behind.valid) {
    vec4 sampleColor = textureLod(savedHdr, behind.uv, 0);
    background = sampleColor.rgb / max(sampleColor.a, 1e-6);
  }
  float surfaceWeight = 0;
  vec3 through;
  float f0 = pow((index - 1) / (index + 1), 2);
  vec3 reflectedWeight = vec3(boundary.reflectance), specularF0 = vec3(f0);
  if (thinSheet) {
    vec2 tintUv = uv + water.scroll2.zw * water.optical.y;
    vec3 tintDepth = -log(clamp(texture(albedoMap, tintUv).rgb, vec3(1e-6), vec3(1))) * water.transmittance.w;
    vec3 opticalDepth = min(water.transmittance.rgb + tintDepth, vec3(80));
    DielectricSheet sheet = dielectricSheet(opticalDepth, 1, abs(dot(n, v)), 1, index);
    through = sheet.transmission;
    reflectedWeight = sheet.reflection;
    specularF0 = dielectricSheet(opticalDepth, 1, 1, 1, index).reflection;
    // Absorption is lost energy, not diffuse body lighting.
  } else
    through = waterThrough(boundary, water.transmittance.rgb, water.transmittance.w,
        behind.distance, behind.valid, cameraInsideWater, surfaceWeight);
  ShadingReceiver receiver = ShadingReceiver(worldPosition, geometric, v, gl_FragCoord.xy, depth,
      BakedReceiver(vec4(0), surfaceBounds, surfaceLights, surfaceDirectLights, surfaceChart), false, 0u, 1);
  float bodyWeight = max(0, surfaceWeight - boundary.reflectance);
  LightingReference reference;
  LightContribution illumination = sampleWaterLighting(receiver, n, surface.rgb, roughness,
                                                       specularF0, bodyWeight, reference);
  ReflectionReceiver reflectionReceiver = ReflectionReceiver(worldPosition, worldPosition, n,
      boundary.reflected, roughness, reference, 0u, water.probeCandidates.xy);
  vec3 reflected = sampleEnvironmentReflection(reflectionReceiver);
  ScreenReflectionView screen = ScreenReflectionView(frame.camera, waterFrame.inverseCamera, frame.view,
      waterFrame.reflectionTrace, waterFrame.reflectionConfig, uvec2(extent));
  vec4 screenHit = traceScreenReflection(screen, worldPosition, n, boundary.reflected, -v,
                                         roughness, gl_FragCoord.xy / vec2(extent));
#ifdef WATER_RAY
  screenHit = waterHardwareReflection(worldPosition, n, boundary.reflected, roughness, screenHit);
#endif
  reflected = resolveReflection(ReflectionSample(screenHit.rgb, screenHit.a), reflected);
  // Bodies retain lit absorption colour; finite sheets lose absorbed energy.
  // Both use the shared filtered reflection path and their optical weights.
  vec3 color = through * background + bodyWeight * illumination.diffuse +
               reflectedWeight * reflected + illumination.specular + (thinSheet ? 1 : surfaceWeight) * emitted;
  result = vec4(clamp(color, vec3(0), vec3(65000)), 1);
}
