// Source 2 traversal uses the chart and cluster lists owned by the local source.
#include "chart-light-list.glsl"
void accumulateStandardLocalLighting(inout vec3 color, Surface surface, ShadingReceiver receiver,
                                     DirectLightingPolicy policy, uint debugView) {
  vec3 n = surface.normal, v = receiver.viewDirection;
  LocalLightingContext localLighting = prepareLocalLighting(receiver, n, surface.metallic < 1, policy.attribution);
  uint lightCount = receiverLocalLightCount(localLighting);
  if (lightCount == 0u) return;
  uint wideVisibilityRow = 0u;
  uint packedHighlights = NoChartLightList;
  bool shadedTraversal = debugView == ViewShaded || debugView >= ViewLocalLightWork;
  if (ChartHighlightLists && shadedTraversal)
    packedHighlights = chartHighlightList(surface, receiver, policy, wideVisibilityRow);
  bool membership = ChartHighlightLists && clusterUsesMembership();
  bool hasChartList = packedHighlights != NoChartLightList;
  uvec4 highlightIds = (uvec4(packedHighlights) >> uvec4(0, 8, 16, 24)) & 255u;
  // For the covered runtime prefix, the cluster header already contains
  // membership in light-index order. A chart mask can narrow it further.
  bool usesPrefix = localLighting.aggregate || MixedLightPrefix == 0u;
  bool useChartMask = membership && usesPrefix;
  uint remaining = useChartMask
      ? chartClusterMembership(localLighting.lights, hasChartList ? packedHighlights : ~0u) : 0u;
#if !defined(VIEWMODEL_PASS) && !defined(WATER_SURFACE) && !defined(LIVE_GI)
  if (useChartMask && clusterConeMasksEnabled() && localLighting.aggregate && indexedSurfaceDirect() &&
      receiver.baked.coordinate.w >= .5 && !localLighting.lights.linear) {
    uint miss = clusters[localLighting.lights.base + 2u];
    if (clusterChartConesEnabled() && receiver.baked.chart < frame.occlusion.y) {
      uint owners = clusters[clusterChartOwnerOffset() + receiver.baked.chart];
      miss |= clusters[localLighting.lights.base + 3u] & ~owners;
    }
    remaining &= ~miss;
  }
#endif
  for (uint i = 0u; useChartMask ? remaining != 0u : i < lightCount; ++i) {
    uint index;
    if (useChartMask) {
      index = uint(findLSB(remaining));
      remaining &= remaining - 1u;
    } else {
      index = localLightIndex(localLighting.lights, i);
    }
    if (ChartHighlightLists && chartRejectsLocalLight(wideVisibilityRow, index)) continue;
    if (ChartHighlightLists && !membership && hasChartList) {
      Light light = lights[index];
      if (localLightHasSelectedHighlight(light) && light.corners[0].w == 0 && light.responseWeights.x > 0 &&
          all(notEqual(highlightIds, uvec4(uint(light.shaping.w)))))
        continue;
    }
    if (debugView == ViewLocalLightMembers) {
      // Runtime-prefix bits in red and green. Blue flags any tail visits.
      if (index < 16u) color[index / 8u] += float(1u << (index % 8u));
      else color.b = 1;
      continue;
    }
    if (debugView == ViewLocalDirectionalOwnership) {
      // Match the aggregate chart query up to the indexed correction lookup.
      Light light = lights[index];
      if (!localLighting.aggregate || !localLightUsesBakedDiffuse(light) ||
          !indexedSurfaceDirect() || receiver.baked.coordinate.w < .5)
        continue;
      vec3 delta = light.positionRadius.xyz - receiver.position;
      if (sqrt(max(dot(delta, delta), 1e-8)) >= light.shaping.z ||
          (dot(n, delta) <= 0 && !emitterAbovePlane(light, receiver.position, n)) ||
          staticLightVisibility(receiver.baked.chart, uint(light.shaping.w)) == 1u ||
          !movingLocalShadowRequired(light, receiver.position))
        continue;
      float irradiance;
      bool owned = surfaceDirectionalField(receiver.baked.chart, uint(light.shaping.w),
          receiver.baked.coordinate, n, receiver.baked.bounds, irradiance);
      color += vec3(1, float(owned), float(owned && irradiance > 0));
      continue;
    }
    uint miss;
    LightContribution local = standardLocalLightByIndex(index, receiver, n, v, surface.color.rgb,
        surface.roughness, surface.metallic, localLighting, vec3(.04), miss);
    if (debugView == ViewLocalLightEarlyMisses || debugView == ViewLocalLightLateMisses) {
      uvec3 reasons = debugView == ViewLocalLightEarlyMisses ? uvec3(1, 2, 3) : uvec3(4, 5, 6);
      color += vec3(equal(uvec3(miss), reasons));
      continue;
    }
    if (debugView == ViewLocalLightWork) {
      LightContribution contribution = selectDirectLight(local, policy);
      bool contributes = any(notEqual(contribution.diffuse, vec3(0))) ||
                         any(notEqual(contribution.specular, vec3(0)));
      bool mixed = contributes && localLighting.aggregate && localLightUsesBakedDiffuse(lights[index]) &&
                   movingLocalShadowRequired(lights[index], receiver.position);
      color += vec3(1, float(contributes), float(mixed));
      continue;
    }
    color += directLightComponent(selectDirectLight(local, policy), debugView);
  }
}
