bool emitterAbovePlane(Light light, vec3 p, vec3 n) {
  if (light.radiance.w <= 0)
    return dot(light.positionRadius.xyz - p, n) + light.positionRadius.w > 0;
  return max(max(dot(light.corners[0].xyz - p, n), dot(light.corners[1].xyz - p, n)),
             dot(light.corners[2].xyz - p, n)) > 0;
}
BrdfLobes localLightResponse(uint index, vec3 p, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                vec4 directIrradiance, vec4 surfaceLightmap, vec3 dielectricF0, vec3 receiverNormal) {
  Light light = lights[index];
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
  uint directChannel = lightChannel(surfaceDirectLights, uint(light.shaping.w));
  bool bakedDiffuse = directChannel < 4u && directIrradiance[min(directChannel, 3u)] >= 0;
  if (!bakedDiffuse && visibility <= 0)
    return BrdfLobes(vec3(0), vec3(0));
  float moving = localShadowVisibility(light, p, receiverNormal, !baked);
  if (moving <= 0)
    return BrdfLobes(vec3(0), vec3(0));
  vec3 l = delta / distance;
  float shape = emitterConeWeight(light.directionCone.xyz, light.directionCone.w,
                                   light.shaping.x, light.shaping.y, -l);
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
  vec3 diffuse = response.diffuse * unitDiffuse;
  vec3 specular = response.specular * (solidAngle * shape * visibility);
  vec3 scale = light.radiance.rgb * (fade * moving);
  return BrdfLobes(diffuse * scale, specular * scale);
}
vec3 localLight(uint index, vec3 p, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                vec4 directIrradiance, vec4 surfaceLightmap, vec3 receiverNormal) {
  BrdfLobes response = localLightResponse(index, p, n, v, base, roughness, metallic,
                                         directIrradiance, surfaceLightmap, vec3(.04), receiverNormal);
  return response.diffuse + response.specular;
}
