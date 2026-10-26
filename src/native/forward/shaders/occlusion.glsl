// Two bits per chart/light pair: unknown=0, blocked=1, clear=2.
// R32_UINT stores each packed word exactly, without byte reconstruction.
layout(set = 0, binding = 9) uniform usampler2D staticLightOcclusion;
uint occlusionWord(uint index) {
  uint width = frame.occlusion.z;
  return texelFetch(staticLightOcclusion, ivec2(index % width, index / width), 0).r;
}
uint staticLightVisibility(uint chart, uint capturedLight) {
  if (chart == 0xffffffffu || capturedLight == 0u)
    return 0u;
  uint stride = frame.occlusion.x & 0x7fffffffu;
  uint light = capturedLight - 1u;
  if (light / 16u >= stride || chart >= frame.occlusion.y)
    return 0u;
  uint index = 2u + chart * stride + light / 16u;
  return (occlusionWord(index) >> ((light % 16u) * 2u)) & 3u;
}

#include "overflow-mask.glsl"
bool overflowLightVisibility(uint chart, uint capturedLight, vec4 coordinate, vec4 bounds,
                              out float visibility) {
  if (!BicubicLightmaps)
    return indexedLightVisibility(frame.occlusion.w, chart, capturedLight, coordinate, bounds, visibility);
  LightmapFilter stencil = surfaceFilter(coordinate, bounds);
  visibility = 0;
  for (uint tap = 0u; tap < 4u; ++tap) {
    float value;
    if (!indexedLightVisibility(frame.occlusion.w, chart, capturedLight,
        vec4(lightmapTap(stencil, tap), coordinate.zw), bounds, value)) {
      visibility = 1;
      return false;
    }
    visibility += lightmapWeight(stencil, tap) * value;
  }
  return true;
}
