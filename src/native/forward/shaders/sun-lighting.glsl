// Receiver-level source query. Packed chart coordinates and shadow bias
// inputs stay here; a material model supplies its own response to the sample.
SunLightSample sampleReceiverSunLight(ShadingReceiver receiver, vec3 normal, float unitIrradiance) {
  return sampleSunLight(receiver.baked.coordinate, receiver.baked.bounds, unitIrradiance,
      receiver.position, receiver.depth, normal, normalize(receiver.geometric));
}
