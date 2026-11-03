// The selected source owns its sky fallback and viewmodel probe bias.
#ifdef SSR_FILTER
#include "environment.glsl"
#elif defined(VIEWMODEL_PASS)
#include "../viewmodel/shaders/environment.glsl"
#endif
vec3 environmentReflectionPosition(vec3 position, vec3 normal) {
#ifdef VIEWMODEL_PASS
  return position + normal * sky.target.z;
#else
  return position;
#endif
}
// Storage and candidate gathering belong to this provider, not material passes.
#include "probes.glsl"
#ifdef SSR_FILTER
#include "probe-tiles.glsl"
uvec2 environmentReflectionCandidates(vec3 position, bool valid) {
  return probeTileCandidates(position, valid);
}
#endif
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
