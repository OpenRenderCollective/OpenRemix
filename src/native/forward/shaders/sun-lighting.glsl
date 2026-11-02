// Receiver-level source query. Packed chart coordinates and shadow bias
// inputs stay here; a material model supplies its own response to the sample.
SunLightSample sampleReceiverSunLight(ShadingReceiver receiver, vec3 normal, float unitIrradiance) {
  return sampleSunLight(receiver.baked.coordinate, receiver.baked.bounds, unitIrradiance,
      receiver.position, receiver.depth, normal, normalize(receiver.geometric));
}

// Optical passes retain the default preset's existing analytic/CSM sunlight policy.
// Keep that choice in the source adapter, not in the water material. A different
// source can supply its own visibility without exposing shadow storage to it.
SunLightSample sampleOpticalReceiverSunLight(ShadingReceiver receiver, vec3 normal) {
  return sampleUnbakedSunLight(receiver.position, receiver.depth, normal, normalize(receiver.geometric));
}
