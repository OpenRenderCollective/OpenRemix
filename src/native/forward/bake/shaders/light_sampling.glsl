#ifdef OFFLINE_LIGHT_SELECTION
// Spatial power proposal, mixed with uniform exploration. This changes only
// the discrete light PDF; emitter sampling and material visibility stay shared.
float lightSelectionWeight(RayLightNode node, vec3 p, vec3 n) {
  vec3 halfSize = .5 * (node.maximum.xyz - node.minimumPower.xyz);
  vec3 delta = .5 * (node.maximum.xyz + node.minimumPower.xyz) - p;
  float radius2 = dot(halfSize, halfSize);
  float extent = sqrt(radius2), facing = dot(delta, n) + extent;
  if (facing <= 0)
    return 0;
  vec3 nearest = max(abs(delta) - halfSize, vec3(0));
  float cosine = min(1, facing * inversesqrt(max(max(dot(delta, delta), radius2), 1e-12)));
  float distance2 = max(dot(nearest, nearest), .01 * radius2);
  return node.minimumPower.w * cosine / max(max(distance2, node.maximum.w), 1e-12);
}
uint selectLight(vec3 p, vec3 n, float u, out float pdf) {
  uint count = rayFrame.counts.y;
  pdf = 1.0 / float(count);
  if ((rayFrame.control.w & 2u) == 0u || count <= 1u)
    return min(uint(u * float(count)), count - 1u);
  const float exploration = .125;
  bool uniformSample = u < exploration;
  uint rank = min(uint(u / exploration * float(count)), count - 1u);
  u = uniformSample ? 0 : (u - exploration) / (1 - exploration);
  uint index = 0u;
  float treePdf = 1;
  // The shared CPU hierarchy is balanced; 32 descents cover its uint32 leaves.
  [[dont_unroll]] for (uint level = 0u; level < 32u; ++level) {
    RayLightNode node = rayLightNodes[index];
    if (node.links.w == 1u) {
      pdf = exploration / float(count) + (1 - exploration) * treePdf;
      return node.links.z;
    }
    RayLightNode left = rayLightNodes[node.links.x], right = rayLightNodes[node.links.y];
    float a = lightSelectionWeight(left, p, n), b = lightSelectionWeight(right, p, n);
    float q = a + b > 0 ? a / (a + b) : float(left.links.w) / float(node.links.w);
    uint rightRank = right.links.w == 1u ? right.links.x : right.links.z;
    bool takeLeft = uniformSample ? rank < rightRank : u < q;
    float branchPdf = takeLeft ? q : 1 - q;
    treePdf *= branchPdf;
    if (!uniformSample)
      u = takeLeft ? u / max(q, 1e-30) : (u - q) / max(1 - q, 1e-30);
    index = takeLeft ? node.links.x : node.links.y;
  }
  // Every valid tree reaches a leaf within the bound.
  RayLightNode leaf = rayLightNodes[index];
  pdf = exploration / float(count) + (1 - exploration) * treePdf;
  return leaf.links.z;
}
#else
uint selectLight(vec3 p, vec3 n, float u, out float pdf) {
  pdf = 1.0 / float(rayFrame.counts.y);
  return min(uint(u * float(rayFrame.counts.y)), rayFrame.counts.y - 1u);
}
#endif
