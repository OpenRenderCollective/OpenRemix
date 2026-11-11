// Function-body template shared by raw sampling and the standard shading model.
// LOCAL_LIGHT_MISS exits using the caller's result type. Keeping that exit in
// the standard function preserves its existing early-rejection control flow.
  vec4 surfaceLightmap = receiver.coordinate;
  Light light = lights[index];
  bool bakedSurface = localLightUsesBakedSurface(light);
  if (aggregateReceiver && bakedSurface && surfaceLightmap.w >= .5)
    LOCAL_LIGHT_MISS(1u);
  bool aggregateDiffuse = aggregateReceiver && localLightUsesBakedDiffuse(light);
  vec3 delta = light.positionRadius.xyz - p;
  float distance2 = max(dot(delta, delta), 1e-8), distance = sqrt(distance2);
  float range = light.shaping.z;
  if (distance >= range)
    LOCAL_LIGHT_MISS(1u);
  // Both analytic lobes vanish below the shading horizon. Retained direct SH
  // can still represent the part of a finite emitter crossing that horizon,
  // so reject only when its entire shape is below the plane as well.
  if (frontFacingOnly && dot(n, delta) <= 0 && !emitterAbovePlane(light, p, n))
    LOCAL_LIGHT_MISS(2u);
  uint cachedVisibility = staticLightVisibility(receiver.chart, uint(light.shaping.w));
  if (cachedVisibility == 1u)
    LOCAL_LIGHT_MISS(3u);
  bool bakedDiffuse = false;
  float attributedDiffuse = 0;
  // An absent legacy field has no usable channel, regardless of ownership.
  // Indexed fields still resolve their own chart/light pair below.
  if (any(greaterThanEqual(directIrradiance, vec4(0)))) {
    uint directChannel = lightChannel(receiver.directLights, uint(light.shaping.w));
    if (directChannel < 4u && directIrradiance[directChannel] >= 0) {
      bakedDiffuse = true;
      attributedDiffuse = directIrradiance[directChannel];
    }
  }
  bool movingShadow = movingLocalShadowRequired(light, p);
  bool probeCorrection = aggregateDiffuse && movingShadow;
  if (indexedSurfaceDirect() && (!aggregateDiffuse || probeCorrection))
    bakedDiffuse = surfaceDirectionalField(receiver.chart, uint(light.shaping.w), surfaceLightmap, n, receiver.bounds, attributedDiffuse);
  vec3 l = delta / distance;
  float shape = preparedEmitterConeWeight(light.directionCone.xyz, light.directionCone.w,
                                   light.shaping.x, light.shaping.y, -l);
  // A zero analytic response needs no filtered masks or shadow filtering.
  // Retained direct SH can still contain a finite emitter crossing the cone
  // boundary even when its centre response vanishes.
  if (shape <= 0 && !bakedDiffuse && !(probeCorrection && surfaceLightmap.w < .5))
    LOCAL_LIGHT_MISS(4u);
  // Clear is recorded only when every receiver sample is exactly one. Its
  // working channel has already been released, so no filtered lookup is needed.
  float visibility = 1;
  bool baked = cachedVisibility == 2u;
  if (!baked)
    baked = bakedLightVisibility(surfaceLightmap, receiver.bounds, receiver.lights,
                                uint(light.shaping.w), visibility);
  if (!baked)
    baked = overflowLightVisibility(receiver.chart, uint(light.shaping.w), surfaceLightmap, receiver.bounds, visibility);
  if (!FullyMappedReceiver && !baked && surfaceLightmap.w < .5) {
    float probeVisibility, probeDiffuse;
    // Aggregate corrections use the same normal-dependent probe stencil as
    // bakedIrradiance; otherwise normal maps could subtract another mixture.
    if (!probeCorrection && batchedProbeVisibility(p, aggregateDiffuse ? n : receiverNormal,
                                                   uint(light.responseWeights.z), probeVisibility)) {
      baked = true;
      visibility = probeVisibility;
    } else if (bakedLocalProbeResponse(p, aggregateDiffuse ? n : receiverNormal, n, uint(light.shaping.w), probeCorrection,
                                 probeVisibility, probeDiffuse)) {
      baked = true;
      visibility = probeVisibility;
      if (probeDiffuse >= 0) {
        bakedDiffuse = true;
        attributedDiffuse = probeDiffuse;
      }
    }
  }
  // Probe SH, like surface SH, may retain a finite emitter crossing the cone.
  if (shape <= 0 && !bakedDiffuse)
    LOCAL_LIGHT_MISS(4u);
  if (!bakedDiffuse && visibility <= 0)
    LOCAL_LIGHT_MISS(5u);
  // A covered bake already contains static visibility. Without a moving
  // caster, its correction is exactly zero: neither unit SH nor PCF is needed.
  uint shadowLayers = (baked ? 0u : StaticLocalShadowLayer) |
                      (movingShadow ? MovingLocalShadowLayer : 0u);
  float moving = localShadowVisibility(light, p, receiverNormal, shadowLayers, true);
  if (moving <= 0 && !aggregateDiffuse)
    LOCAL_LIGHT_MISS(6u);
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
  float fade = emitterRangeFade(distance, range, light.corners[2].x, light.radiance.w > 0);
  // Truncated SH can ring below the receiver plane. Clip a wholly hidden
  // emitter analytically while allowing a finite emitter to cross the horizon.
  float receiverCosine = max(dot(n, l), 0);
  float unitDiffuse = bakedDiffuse ? (emitterAbovePlane(light, p, n) ? attributedDiffuse : 0)
                                   : receiverCosine * solidAngle * shape * visibility;
  // Missing attributed coverage is not evidence that the aggregate contains
  // this analytic estimate. Do not subtract unproven energy from other lights.
  // Automatic aggregate selection must supply correction coverage first.
  if (aggregateDiffuse && !baked && !bakedDiffuse)
    unitDiffuse = 0;
  LocalLightSample lighting = LocalLightSample(aggregateDiffuse, aggregateReceiver && localLightHasSelectedHighlight(light),
      l, light.radiance.rgb, unitDiffuse, solidAngle * shape * visibility,
      moving, fade, light.responseWeights.x);
