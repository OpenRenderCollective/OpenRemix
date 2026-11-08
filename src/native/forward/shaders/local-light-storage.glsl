#ifndef OPENREMIX_LOCAL_LIGHT_STORAGE
#define OPENREMIX_LOCAL_LIGHT_STORAGE
// Default local-light records and clustered enumeration. The cluster builder
// and selected local source share this layout; material passes need not import it.
#include "local-light-record.glsl"
layout(set = 0, binding = 1, std430) readonly buffer Lights {
  Light lights[];
};
#ifdef CLUSTER_BUILD
layout(set = 0, binding = 2, std430) buffer Clusters {
  uint clusters[];
};
#else
layout(set = 0, binding = 2, std430) readonly buffer Clusters {
  uint clusters[];
};
#endif
#define ClusterCapacity frame.clusterInfo.x
bool clusterConeMasksEnabled() { return (frame.clusterInfo.w & 128u) != 0u; }
bool clusterChartConesEnabled() { return (frame.clusterInfo.w & 2048u) != 0u; }
#define ClusterHeaderWords (2u + uint(clusterConeMasksEnabled()) + uint(clusterChartConesEnabled()))
#define ClusterStride (ClusterCapacity + ClusterHeaderWords)
#define MixedLightPrefix (frame.clusterInfo.y & 0x7fffffffu)
#define ClusterLightCount (MixedLightPrefix == 0u ? frame.target.z : MixedLightPrefix - 1u)
#define ClusterBakedTail ((frame.clusterInfo.y & 0x80000000u) != 0u)

// The high bit of the chart table control selects runtime membership in the first
// cluster header word. Larger lists retain the ordinary prefix count.
// These flags match mixed::ChartHighlightField.
const uint ClusterMembershipFlag = 1u << 31u;
const uint ChartExactVisibilityFlag = 1u << 30u;
const uint ChartTableOffsetMask = ~(ClusterMembershipFlag | ChartExactVisibilityFlag);
bool clusterUsesMembership() { return (frame.occlusion.z & ClusterMembershipFlag) != 0u; }
// Lights are ordered runtime prefix, then baked tail. Each cell stores the
// prefix count and complete count, followed by their shared ordered indices.
// Tail overflow must not force covered receivers to evaluate baked lights.
uint receiverLightCount(uint base, bool aggregateReceiver, out bool linear) {
  bool fallback = !aggregateReceiver && MixedLightPrefix != 0u;
  if (fallback && !ClusterBakedTail) {
    linear = true;
    return frame.target.z;
  }
  uint count = clusters[base + uint(fallback)];
  if (!fallback && clusterUsesMembership()) count = uint(bitCount(count));
  linear = count > ClusterCapacity;
  return linear ? (fallback ? frame.target.z : ClusterLightCount) : count;
}
uint clusterChartOwnerOffset() { return frame.grid.w * ClusterStride; }
float sliceDepth(uint slice) {
  return frame.depth.x * pow(frame.depth.y / frame.depth.x, float(slice) / float(frame.grid.z));
}
uint clusterIndex(vec2 pixel, float depth) {
  uvec2 tile = min(uvec2(pixel) * frame.grid.xy / frame.target.xy, frame.grid.xy - 1u);
  uint z = uint(
      clamp(log(max(depth, frame.depth.x) / frame.depth.x) * frame.depth.z, 0, float(frame.grid.z - 1u)));
  return (z * frame.grid.y + tile.y) * frame.grid.x + tile.x;
}
#endif
