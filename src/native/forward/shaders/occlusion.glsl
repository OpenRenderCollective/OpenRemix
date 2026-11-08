// Two bits per chart/light pair: unknown=0, blocked=1, clear=2.
// R32_UINT stores each packed word exactly, without byte reconstruction.
layout(set = 0, binding = 9) uniform usampler2DArray staticLightOcclusion;
const uint occlusionPageShift = 10u; // 1024, mirrored in mixed/occlusion.h
uint occlusionWord(uint index) {
  const uint mask = (1u << occlusionPageShift) - 1u;
  uint row = index >> occlusionPageShift;
  return texelFetch(staticLightOcclusion,
      ivec3(index & mask, row & mask, row >> occlusionPageShift), 0).r;
}
uint staticLightVisibility(uint chart, uint capturedLight) {
  if (chart == 0xffffffffu || capturedLight == 0u)
    return 0u;
  uint stride = frame.occlusion.x & 0x1fffffffu;
  uint light = capturedLight - 1u;
  if (light / 16u >= stride || chart >= frame.occlusion.y)
    return 0u;
  uint index = 2u + chart * stride + light / 16u;
  return (occlusionWord(index) >> ((light % 16u) * 2u)) & 3u;
}

#include "overflow-mask.glsl"
bool overflowLightVisibility(uint chart, uint capturedLight, vec4 coordinate, vec4 bounds,
                              out float visibility) {
  visibility = 1;
  // All cubic taps share one chart/light record. Resolve ownership and storage
  // metadata once; only their bilinear sample coordinates differ.
  VisibilityField field;
  if (!indexedVisibilityField(frame.occlusion.w, chart, capturedLight, coordinate, field)) return false;
  if (!BicubicLightmaps) {
    visibility = sampleVisibilityField(frame.occlusion.w, field, coordinate, bounds);
    return true;
  }
  LightmapFilter stencil = surfaceFilter(coordinate, bounds);
  visibility = 0;
  for (uint tap = 0u; tap < 4u; ++tap) {
    float value = sampleVisibilityField(frame.occlusion.w, field,
        vec4(lightmapTap(stencil, tap), coordinate.zw), bounds);
    visibility += lightmapWeight(stencil, tap) * value;
  }
  return true;
}
