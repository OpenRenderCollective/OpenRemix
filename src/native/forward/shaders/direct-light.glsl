#include "diffuse-residual.glsl"
bool emitterAbovePlane(Light light, vec3 p, vec3 n) {
  if (light.radiance.w <= 0)
    return dot(light.positionRadius.xyz - p, n) + light.positionRadius.w > 0;
  return max(max(dot(light.corners[0].xyz - p, n), dot(light.corners[1].xyz - p, n)),
             dot(light.corners[2].xyz - p, n)) > 0;
}
BrdfLobes localLightResponse(uint index, vec3 p, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                vec4 directIrradiance, vec4 surfaceLightmap, vec3 dielectricF0, vec3 receiverNormal,
                bool aggregateReceiver) {
  Light light = lights[index];
  bool aggregateDiffuse = aggregateReceiver && light.corners[2].w > .5;
  vec3 delta = light.positionRadius.xyz - p;
  float distance2 = max(dot(delta, delta), 1e-8), distance = sqrt(distance2);
  float range = light.shaping.z;
  if (distance >= range)
    return BrdfLobes(vec3(0), vec3(0));
  // Both analytic lobes vanish below the shading horizon. Retained direct SH
  // can still represent the part of a finite emitter crossing that horizon,
  // so reject only when its entire shape is below the plane as well.
  if (dot(n, delta) <= 0 && !emitterAbovePlane(light, p, n))
    return BrdfLobes(vec3(0), vec3(0));
  uint cachedVisibility = staticLightVisibility(surfaceChart, uint(light.shaping.w));
  if (cachedVisibility == 1u)
    return BrdfLobes(vec3(0), vec3(0));
  uint directChannel = lightChannel(surfaceDirectLights, uint(light.shaping.w));
  bool bakedDiffuse = directChannel < 4u && directIrradiance[min(directChannel, 3u)] >= 0;
  vec3 l = delta / distance;
  float shape = emitterConeWeight(light.directionCone.xyz, light.directionCone.w,
                                   light.shaping.x, light.shaping.y, -l);
  // A zero analytic response needs no filtered masks or shadow filtering.
  // Retained direct SH can still contain a finite emitter crossing the cone
  // boundary even when its centre response vanishes.
  if (shape <= 0 && !bakedDiffuse)
    return BrdfLobes(vec3(0), vec3(0));
  float visibility;
  uint channel;
  bool baked =
      bakedLightVisibility(surfaceLightmap, surfaceLights, uint(light.shaping.w), visibility, channel);
  baked = baked || cachedVisibility == 2u;
  if (!baked)
    baked = overflowLightVisibility(surfaceChart, uint(light.shaping.w), surfaceLightmap, surfaceBounds, visibility);
  if (!baked && surfaceLightmap.w < .5) {
    float probeVisibility;
    if (bakedLocalProbeVisibility(p, receiverNormal, uint(light.shaping.w), probeVisibility)) {
      baked = true;
      visibility = probeVisibility;
    }
  }
  if (!bakedDiffuse && visibility <= 0)
    return BrdfLobes(vec3(0), vec3(0));
  float moving = localShadowVisibility(light, p, receiverNormal, !baked);
  if (moving <= 0 && !aggregateDiffuse)
    return BrdfLobes(vec3(0), vec3(0));
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
    solidAngle = 2 * Pi * sphereOneMinusCos(distance2, light.positionRadius.w);
  float fade = pow(clamp(1 - pow(distance / range, 4), 0, 1), 2);
  BrdfLobes response = brdfLobes(base, roughness, metallic, n, v, l, dielectricF0);
  // Truncated SH can ring below the receiver plane. Clip a wholly hidden
  // emitter analytically while allowing a finite emitter to cross the horizon.
  float unitDiffuse = bakedDiffuse ? (emitterAbovePlane(light, p, n) ? directIrradiance[directChannel] : 0)
                                   : max(dot(n, l), 0) * solidAngle * shape * visibility;
  // Missing attributed coverage is not evidence that the aggregate contains
  // this analytic estimate. Do not subtract unproven energy from other lights.
  // Automatic aggregate selection must supply correction coverage first.
  if (aggregateDiffuse && !baked && !bakedDiffuse)
    unitDiffuse = 0;
  // The aggregate SH is irradiance, shaded by the same Lambertian material
  // weight as the ambient term. Keep its moving-caster correction in those
  // units instead of adding another view-dependent analytic diffuse lobe.
  vec3 diffuseWeight = aggregateDiffuse ? base * (1 - metallic) / Pi : response.diffuse;
  vec3 diffuse = diffuseResidual(diffuseWeight * unitDiffuse, moving, aggregateDiffuse);
  vec3 specular = response.specular * (solidAngle * shape * visibility);
  // SelectedAggregate (LightResponse = 3) follows the shadow slot's existing
  // fade. Outside baked coverage this is still a complete analytic fallback.
  if (aggregateReceiver && light.corners[2].w == 3)
    specular *= 1 - light.corners[1].w;
  // Aggregate transport integrates the physical emitter without the runtime
  // influence fade. Its subtraction must use those same radiometric units.
  return BrdfLobes(diffuse * light.radiance.rgb * (aggregateDiffuse ? 1 : fade),
                   specular * light.radiance.rgb * (fade * moving));
}
vec3 localLight(uint index, vec3 p, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                vec4 directIrradiance, vec4 surfaceLightmap, vec3 receiverNormal) {
  BrdfLobes response = localLightResponse(index, p, n, v, base, roughness, metallic,
                                         directIrradiance, surfaceLightmap, vec3(.04), receiverNormal, false);
  return response.diffuse + response.specular;
}
