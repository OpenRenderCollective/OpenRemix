// The selected source owns its sky fallback and viewmodel probe bias.
#ifdef SSR_FILTER
#include "environment.glsl"
#elif defined(VIEWMODEL_PASS) || defined(INLINE_WORLD_REFLECTIONS)
#include "scene-environment.glsl"
#elif defined(WATER_SURFACE)
#include "../water/shaders/environment.glsl"
#endif
vec3 environmentReflectionPosition(vec3 position, vec3 normal) {
#ifdef VIEWMODEL_PASS
  return position + normal * sky.target.z;
#elif defined(INLINE_WORLD_REFLECTIONS)
  return fma(normal, vec3(sky.target.z), position);
#else
  return position;
#endif
}
// Storage and candidate gathering belong to this provider, not material passes.
#include "probes.glsl"
#ifdef SSR_FILTER
#include "probe-tiles.glsl"
uvec2 environmentReflectionCandidates(vec3 position, bool valid, uint layer) {
  // Explicit bindings skip volume selection. Stale layer IDs still need the
  // shared candidate list so they can fall back to automatic selection.
  return probeTileCandidates(position, valid && !probeLayerAvailable(layer));
}
#endif
// Source 2 environment source: authored/automatic parallax probes, then sky.
// Implementations return scene-linear incident radiance, before material Fresnel.
vec3 sampleEnvironmentReflection(ReflectionReceiver receiver) {
  vec3 radiance;
  float referenceLuma = lightingReferenceLuminance(receiver.reference, Source2StaticLightingReference);
  if (boundProbeRadiance(receiver.layer, receiver.probePosition, receiver.direction,
                         receiver.roughness, receiver.normal, referenceLuma, radiance))
    return radiance;
#ifdef MASKED_REFLECTION_PROBES
  if (probeRadianceMasked(receiver.probePosition, receiver.direction, receiver.roughness,
                          receiver.candidates, receiver.normal, referenceLuma, radiance))
#else
  if (probeRadiance(receiver.probePosition, receiver.direction, receiver.roughness,
                    receiver.normal, referenceLuma, radiance))
#endif
    return radiance;
  return environmentReflection(receiver.position, receiver.normal, receiver.direction, receiver.roughness);
}
