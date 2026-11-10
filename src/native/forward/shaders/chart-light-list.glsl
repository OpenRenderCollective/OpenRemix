// Standard receiver chart selection and its intersection with the runtime light list.
layout(constant_id = 7) const bool ChartHighlightLists = false;
const uint NoChartLightList = ~0u;
bool chartRejectsLocalLight(uint row, uint index) {
  // Exact visibility is projected onto the runtime prefix only.
  return row != 0u && index < ClusterLightCount &&
      (clusters[row + index / 32u] & (1u << (index % 32u))) == 0u;
}
uint chartHighlightList(Surface surface, ShadingReceiver receiver, DirectLightingPolicy policy,
                        out uint wideVisibilityRow) {
  wideVisibilityRow = 0u;
  uint storage = frame.occlusion.z;
  uint table = storage & ChartTableOffsetMask;
  BakedReceiver baked = receiver.baked;
  if (table == 0u || !policy.attribution || baked.coordinate.w < .5 || baked.chart >= frame.occlusion.y)
    return NoChartLightList;
  if ((storage & ChartExactVisibilityFlag) != 0u) {
    if (clusterUsesMembership()) return clusters[table + baked.chart];
    wideVisibilityRow = table + baked.chart * ((ClusterLightCount + 31u) / 32u);
    return NoChartLightList;
  }
  if (receiver.opacity < 1 || surface.roughness < .65 || !policy.specular) return NoChartLightList;
  vec2 size = vec2(textureSize(surfaceAtlas, 0).xy);
  uvec2 tileCount = (uvec2(round((baked.bounds.zw - baked.bounds.xy) * size)) + 8u) / 8u;
  uvec2 tile = min(uvec2(max((baked.coordinate.xy - baked.bounds.xy) * size, vec2(0))) / 8u, tileCount - 1u);
  return clusters[table + clusters[table + baked.chart] + tile.y * tileCount.x + tile.x];
}
uint chartClusterMembership(LocalLightList list, uint visible) {
  if (list.linear) {
    // Exact chart masks also contain bits for unused runtime slots.
    uint count = ClusterLightCount;
    uint prefix = count >= 32u ? ~0u : (1u << count) - 1u;
    return visible & prefix;
  }
  return visible & clusters[list.base];
}
