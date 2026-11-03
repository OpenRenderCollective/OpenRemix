// Function-body template shared by raw sampling and the standard shading model.
// LOCAL_LIGHT_MISS exits using the caller's result type. Keeping that exit in
// the standard function preserves its existing early-rejection control flow.
// LOCAL_LIGHT_FINAL_RESPONSE may defer work that cannot affect that response;
// raw samplers must not define it.
  vec4 surfaceLightmap = receiver.coordinate;
  Light light = lights[index];
  bool aggregateDiffuse = aggregateReceiver && light.corners[2].w > .5;
  vec3 delta = light.positionRadius.xyz - p;
  float distance2 = max(dot(delta, delta), 1e-8), distance = sqrt(distance2);
  float range = light.shaping.z;
  if (distance >= range)
    LOCAL_LIGHT_MISS;
  // Both analytic lobes vanish below the shading horizon. Retained direct SH
  // can still represent the part of a finite emitter crossing that horizon,
  // so reject only when its entire shape is below the plane as well.
  if (frontFacingOnly && dot(n, delta) <= 0 && !emitterAbovePlane(light, p, n))
    LOCAL_LIGHT_MISS;
  uint cachedVisibility = staticLightVisibility(receiver.chart, uint(light.shaping.w));
  if (cachedVisibility == 1u)
    LOCAL_LIGHT_MISS;
  uint directChannel = lightChannel(receiver.directLights, uint(light.shaping.w));
  bool bakedDiffuse = directChannel < 4u && directIrradiance[min(directChannel, 3u)] >= 0;
  bool movingShadow = movingLocalShadowRequired(light);
#ifdef ZERO_DIFFUSE_MATERIAL
  // Scalar visibility and specular remain; this material has no diffuse energy.
  bool probeCorrection = false;
#else
  bool probeCorrection = aggregateDiffuse && movingShadow;
#endif
#if defined(LOCAL_LIGHT_FINAL_RESPONSE) && !defined(ZERO_DIFFUSE_MATERIAL)
#define LOCAL_LIGHT_DEFER_DIRECTIONAL
  // Only the final standard response can discard a zero mixed residual. Raw
  // queries retain full unit irradiance, even for an unshadowed receiver.
  bool deferredDirectional = indexedSurfaceDirect() && probeCorrection && surfaceLightmap.w >= .5;
#define LOCAL_LIGHT_REJECT_IF_READY(condition) if ((condition) && !deferredDirectional) LOCAL_LIGHT_MISS
#else
#define LOCAL_LIGHT_REJECT_IF_READY(condition) if (condition) LOCAL_LIGHT_MISS
#endif
  float attributedDiffuse = bakedDiffuse ? directIrradiance[min(directChannel, 3u)] : 0;
  if (indexedSurfaceDirect() && (!aggregateDiffuse || probeCorrection)
#ifdef LOCAL_LIGHT_DEFER_DIRECTIONAL
      && !deferredDirectional
#endif
      )
    bakedDiffuse = surfaceDirectionalField(receiver.chart, uint(light.shaping.w), surfaceLightmap, n, receiver.bounds, attributedDiffuse);
  vec3 l = delta / distance;
  float shape = emitterConeWeight(light.directionCone.xyz, light.directionCone.w,
                                   light.shaping.x, light.shaping.y, -l);
  // A zero analytic response needs no filtered masks or shadow filtering.
  // Retained direct SH can still contain a finite emitter crossing the cone
  // boundary even when its centre response vanishes.
  LOCAL_LIGHT_REJECT_IF_READY(shape <= 0 && !bakedDiffuse && !(probeCorrection && surfaceLightmap.w < .5));
  // Clear is recorded only when every receiver sample is exactly one. Its
  // working channel has already been released, so no filtered lookup is needed.
  float visibility = 1;
  uint channel;
  bool baked = cachedVisibility == 2u;
  if (!baked)
    baked = bakedLightVisibility(surfaceLightmap, receiver.bounds, receiver.lights,
                                uint(light.shaping.w), visibility, channel);
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
  LOCAL_LIGHT_REJECT_IF_READY(shape <= 0 && !bakedDiffuse);
  LOCAL_LIGHT_REJECT_IF_READY(!bakedDiffuse && visibility <= 0);
  // A covered bake already contains static visibility. Without a moving
  // caster, its correction is exactly zero: neither unit SH nor PCF is needed.
  float moving = baked && !movingShadow ? 1 : localShadowVisibility(light, p, receiverNormal, !baked);
#ifdef LOCAL_LIGHT_DEFER_DIRECTIONAL
  if (deferredDirectional) {
    // Exact equality only: do not turn small PCF residuals into fully lit ones.
    if (moving != 1)
      bakedDiffuse = surfaceDirectionalField(receiver.chart, uint(light.shaping.w),
          surfaceLightmap, n, receiver.bounds, attributedDiffuse);
    // A missing field cannot retain finite-emitter energy outside the analytic
    // cone. These guards now run after the deferred ownership decision.
    if (shape <= 0 && !bakedDiffuse)
      LOCAL_LIGHT_MISS;
    if (!bakedDiffuse && visibility <= 0)
      LOCAL_LIGHT_MISS;
  }
#undef LOCAL_LIGHT_DEFER_DIRECTIONAL
#endif
#undef LOCAL_LIGHT_REJECT_IF_READY
  if (moving <= 0 && !aggregateDiffuse)
    LOCAL_LIGHT_MISS;
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
  LocalLightSample lighting = LocalLightSample(aggregateDiffuse, aggregateReceiver && light.corners[2].w == 3,
      l, light.radiance.rgb, unitDiffuse, solidAngle * shape * visibility,
      moving, fade, light.responseWeights.x);
