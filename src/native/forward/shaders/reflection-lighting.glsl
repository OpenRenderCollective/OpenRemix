// Incident environment radiance, before Fresnel or screen/ray confidence.
// The pass supplies its existing probe-position bias and candidate set. Material
// shading does not depend on probe atlas layout or authored/fitted selection.
struct ReflectionReceiver {
  vec3 position;
  vec3 probePosition;
  vec3 normal;
  vec3 direction;
  float roughness;
  float irradianceLuma;
  uint layer;
  uvec2 candidates;
};
vec3 sampleEnvironmentReflection(ReflectionReceiver receiver) {
  vec3 radiance;
#ifdef PROBE_TILE_CANDIDATES
  if (!boundProbeRadiance(receiver.layer, receiver.probePosition, receiver.direction,
                          receiver.roughness, receiver.normal, receiver.irradianceLuma, radiance) &&
      !probeRadianceMasked(receiver.probePosition, receiver.direction, receiver.roughness,
                           receiver.candidates, receiver.normal, receiver.irradianceLuma, radiance))
#else
  if (!probeRadiance(receiver.probePosition, receiver.direction, receiver.roughness,
                     receiver.normal, receiver.irradianceLuma, radiance))
#endif
    radiance = environmentReflection(receiver.position, receiver.normal, receiver.direction, receiver.roughness);
  return radiance;
}
