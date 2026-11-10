// One reconstructed group per invocation, reused by adjacent selected lights.
// Attributed moving corrections retain the original directional lookup.
#if !defined(WATER_SURFACE) && !defined(LIVE_GI)
layout(constant_id = 5) const bool BatchedProbeVisibility = false;
layout(constant_id = 8) const bool ProbeEndpointCells = false;
layout(set = 0, binding = 22, std430) readonly buffer SelectedProbeVisibility {
  uvec4 selectedProbeHeader;
  vec4 selectedProbeMasks[];
};
uint selectedProbeGroup = ~0u, selectedProbeRevision = ~0u;
vec4 selectedProbeValues;
uint probeEndpointCode(uint cell, uint group) {
  uint entry = cell * selectedProbeHeader.z + group;
  uint packed = uint(selectedProbeMasks[selectedProbeHeader.x * selectedProbeHeader.z + entry / 16u][entry / 4u % 4u]);
  return (packed >> ((entry % 4u) * 5u)) & 31u;
}
bool probeEndpointGroup(uint group, out vec4 values) {
  bool coarse = localStencilCellKeys.x != 0xffffffffu;
  bool fine = localStencilCount == 16u;
  uint code = coarse ? probeEndpointCode(localStencilCellKeys.x, group)
                     : fine ? probeEndpointCode(localStencilCellKeys.y, group) : 0u;
  if (code == 0u || (coarse && fine && code != probeEndpointCode(localStencilCellKeys.y, group)))
    return false;
  values = vec4((uvec4(code - 1u) >> uvec4(0, 1, 2, 3)) & 1u);
  return true;
}
bool batchedProbeVisibility(vec3 position, vec3 normal, uint slot, out float visibility) {
  if (!BatchedProbeVisibility || slot == 0u) return false;
  if ((selectedProbeHeader.w & ~1u) != 0x56504252u || selectedProbeHeader.x != bakeDimensions.w ||
      slot > selectedProbeHeader.y) return false;
  if (!localProbeStencil(position, normal)) return false;
  uint group = (slot - 1u) / 4u;
  if (selectedProbeGroup != group || selectedProbeRevision != localStencilRevision) {
    if (ProbeEndpointCells && (selectedProbeHeader.w & 1u) != 0u && probeEndpointGroup(group, selectedProbeValues)) {
      selectedProbeGroup = group;
      selectedProbeRevision = localStencilRevision;
      visibility = selectedProbeValues[(slot - 1u) % 4u];
      return true;
    }
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
