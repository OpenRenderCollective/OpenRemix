// One reconstructed group per invocation, reused by adjacent selected lights.
// Attributed moving corrections retain the original directional lookup.
#if !defined(WATER_SURFACE) && !defined(LIVE_GI)
layout(constant_id = 5) const bool BatchedProbeVisibility = false;
layout(set = 0, binding = 22, std430) readonly buffer SelectedProbeVisibility {
  uvec4 selectedProbeHeader;
  vec4 selectedProbeMasks[];
};
uint selectedProbeGroup = ~0u, selectedProbeRevision = ~0u;
vec4 selectedProbeValues;
bool batchedProbeVisibility(vec3 position, vec3 normal, uint slot, out float visibility) {
  if (!BatchedProbeVisibility || slot == 0u) return false;
  if (selectedProbeHeader.w != 0x56504252u || selectedProbeHeader.x != bakeDimensions.w ||
      slot > selectedProbeHeader.y) return false;
  if (!localProbeStencil(position, normal)) return false;
  uint group = (slot - 1u) / 4u;
  if (selectedProbeGroup != group || selectedProbeRevision != localStencilRevision) {
    vec4 sum = vec4(0), total = vec4(0);
    bvec4 complete = bvec4(true);
    for (uint corner = 0u; corner < localStencilCount; ++corner) {
      float weight = localStencilWeights[corner];
      if (weight <= 0) continue;
      vec4 masks = selectedProbeMasks[localStencilIndices[corner] * selectedProbeHeader.z + group];
      bvec4 known = greaterThanEqual(masks, vec4(0));
      if (weight > 1e-6) complete = bvec4(complete.x && known.x, complete.y && known.y,
                                       complete.z && known.z, complete.w && known.w);
      sum += weight * max(masks, vec4(0));
      total += weight * vec4(known);
    }
    selectedProbeValues = mix(vec4(-1), clamp(sum / max(total, vec4(1e-20)), 0, 1),
        bvec4(complete.x && total.x >= 1e-4, complete.y && total.y >= 1e-4,
              complete.z && total.z >= 1e-4, complete.w && total.w >= 1e-4));
    selectedProbeGroup = group;
    selectedProbeRevision = localStencilRevision;
  }
  visibility = selectedProbeValues[(slot - 1u) % 4u];
  return visibility >= 0;
}
#else
bool batchedProbeVisibility(vec3 position, vec3 normal, uint slot, out float visibility) {
  return false;
}
#endif
