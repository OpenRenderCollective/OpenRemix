// Receiver-local enumeration. Storage layout and overflow policy stay behind
// this interface; shading models consume a count and resolve each light index.
struct LocalLightList {
  uint base;
  uint count;
  bool linear;
};
LocalLightList localLightsForReceiver(ShadingReceiver receiver, bool aggregate) {
  // Covered receivers enumerate only the live prefix. A baked-only tail
  // still serves uncovered receivers, which must keep their fallback list.
  if ((aggregate ? ClusterLightCount : frame.target.z) == 0u)
    return LocalLightList(0u, 0u, false);
  uint cell = clusterIndex(receiver.screenPosition, receiver.depth);
  uint base = cell * ClusterStride;
  bool linear;
  uint count = receiverLightCount(base, aggregate, linear);
  return LocalLightList(base, count, linear);
}
uint localLightIndex(LocalLightList list, uint ordinal) {
  if (list.linear) return ordinal;
  return clusters[list.base + ClusterHeaderWords + ordinal];
}
