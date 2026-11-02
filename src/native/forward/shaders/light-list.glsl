// Receiver-local enumeration. Storage layout and overflow policy stay behind
// this interface; shading models consume a count and resolve each light index.
struct LocalLightList {
  uint base;
  uint count;
  bool linear;
};
LocalLightList localLightsForReceiver(ShadingReceiver receiver, bool aggregate) {
  uint base = clusterIndex(receiver.screenPosition, receiver.depth) * ClusterStride;
  bool linear;
  uint count = receiverLightCount(base, aggregate, linear);
  return LocalLightList(base, count, linear);
}
uint localLightIndex(LocalLightList list, uint ordinal) {
  return list.linear ? ordinal : clusters[list.base + ClusterHeaderWords + ordinal];
}
