// Source 2 environment source: authored/automatic parallax probes, then sky.
// Implementations return scene-linear incident radiance, before material Fresnel.
vec3 sampleEnvironmentReflection(ReflectionReceiver receiver) {
  vec3 radiance;
  float referenceLuma = lightingReferenceLuminance(receiver.reference, Source2StaticLightingReference);
#ifdef PROBE_TILE_CANDIDATES
  if (!boundProbeRadiance(receiver.layer, receiver.probePosition, receiver.direction,
                          receiver.roughness, receiver.normal, referenceLuma, radiance) &&
      !probeRadianceMasked(receiver.probePosition, receiver.direction, receiver.roughness,
                           receiver.candidates, receiver.normal, referenceLuma, radiance))
#else
  if (!probeRadiance(receiver.probePosition, receiver.direction, receiver.roughness,
                     receiver.normal, referenceLuma, radiance))
#endif
    radiance = environmentReflection(receiver.position, receiver.normal, receiver.direction, receiver.roughness);
  return radiance;
}
