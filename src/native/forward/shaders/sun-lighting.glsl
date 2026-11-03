#include "baked-storage.glsl"
#include "sun.glsl"
// Receiver-level source query. Packed chart coordinates and shadow bias
// inputs stay here; a material model supplies its own response to the sample.
SunLightSample sampleReceiverSunLight(ShadingReceiver receiver, vec3 normal, float unitIrradiance) {
  return sampleSunLight(receiver.baked.coordinate, receiver.baked.bounds, unitIrradiance,
      receiver.position, receiver.depth, normal, normalize(receiver.geometric));
}

// Keep optical visibility policy in the source adapter, not the water material.
// Composed receivers share the dynamic-probe sun mask and moving-only shadows;
// missing coverage retains complete static CSM fallback. The default preset's
// existing analytic/CSM policy is unchanged.
SunLightSample sampleOpticalReceiverSunLight(ShadingReceiver receiver, vec3 normal) {
#ifdef OPENREMIX_BAKED_DIRECT_LIVE_INDIRECT
  return sampleSunLight(vec4(0), vec4(0), 0, receiver.position, receiver.depth, normal,
      normalize(receiver.geometric));
#else
  return sampleUnbakedSunLight(receiver.position, receiver.depth, normal, normalize(receiver.geometric));
#endif
}
