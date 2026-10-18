// Pinned NVIDIA SHARC 1.8.3.0. See third_party/sharc/License.md.
#include "../sharc_policy.h"
#define SHARC_ENABLE_GLSL 1
#ifdef SHARC_UPDATE_PASS
#define SHARC_UPDATE 1
#else
#define SHARC_UPDATE 0
#endif
#define SHARC_MATERIAL_DEMODULATION 1
#define SHARC_ENABLE_SH_ENCODING 1
#define SHARC_ENABLE_CACHE_RESAMPLING 1
#define SHARC_SAMPLE_NUM_THRESHOLD 3
// The resolve keeps the SDK's reads of other entries, as dxvk-remix does: a
// cell new at its level starts from the adjacent level's radiance while the
// camera moves (SHARC_BLEND_ADJACENT_LEVELS), and a key inserted again at an
// earlier slot takes over its old entry (SHARC_LINEAR_PROBE_WINDOW_SIZE).
// Those reads race with the entry's own invocation and see it before or after
// its resolve; either is a valid history. Without them every change of level
// was a band of cache misses until the cell had its samples again.
// NRD's HLSL compatibility aliases are already expanded in its declarations.
#undef rcp
#undef saturate
#undef lerp
#undef countbits
#undef asfloat
#undef asuint
#undef f32tof16
#undef f16tof32
#include "SharcGlslHelpers.h"
#include "SharcCommon.h"
layout(buffer_reference,std430,buffer_reference_align=4) buffer CacheCounters { uint data[]; };
SharcParameters cacheParameters() {
  SharcParameters p;
  p.hashGridParameters.cameraPosition=frame.eye.xyz;
  p.hashGridParameters.logarithmBase=2;
  p.hashGridParameters.sceneScale=frame.sharcGrid.x;
  p.hashGridParameters.levelBias=0;
  p.hashGridData.capacity=frame.sharcConfig.y;
  p.hashGridData.hashEntriesBuffer=RWStructuredBuffer_uint64_t(frame.sharcHash);
#if defined(HASH_GRID_ENABLE_64_BIT_ATOMICS) && !HASH_GRID_ENABLE_64_BIT_ATOMICS
  // Without 64-bit atomics (the Metal backend) SHARC serializes insertion with
  // a 32-bit lock per entry, allocated after the hash entries.
  p.hashGridData.lockBuffer=RWStructuredBuffer_uint(frame.sharcHash+uint64_t(frame.sharcConfig.y)*8);
#endif
  p.radianceScale=1000;
  p.accumulationBuffer=RWStructuredBuffer_SharcAccumulationData(frame.sharcAccumulation);
  p.resolvedBuffer=RWStructuredBuffer_SharcPackedData(frame.sharcResolved);
  return p;
}
