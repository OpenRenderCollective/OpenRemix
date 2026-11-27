// Select complete receiver contexts with their diffuse implementation. Materials
// consume the same queries without depending on chart or ray representations.
#include "baked-storage.glsl"
#include "diffuse-values.glsl"
// The coverage view inspects receiver ownership without filtering irradiance.
vec3 receiverBakedCoverage(ShadingReceiver receiver, vec3 normal) {
  if (hasSurfaceIrradiance(receiver.baked.coordinate)) return vec3(0, 0, 1);
  return localProbeStencil(receiver.position, normal) ? vec3(0, 1, 0) : vec3(1, 0, 0);
}
#ifdef OPENREMIX_BAKED_DIRECT_LIVE_INDIRECT
// Composition depends on the incoming query, not the provider's ray capability.
#include "diffuse-composed.glsl"
#include "receiver-baked.glsl"
#elif defined(SPLIT_CASCADES)
// A live source for every other purpose (LIVE_GI), reading probes instead of tracing.
#include "diffuse-cascades.glsl"
#include "receiver-live.glsl"
#elif defined(LIVE_GI)
#include "diffuse-ray.glsl"
#include "receiver-live.glsl"
#else
#include "diffuse-baked.glsl"
#include "receiver-baked.glsl"
#endif
