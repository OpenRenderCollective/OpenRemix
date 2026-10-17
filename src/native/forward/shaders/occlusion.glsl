// Two bits per chart/light pair: unknown=0, blocked=1, clear=2.
// Each unfiltered RGBA16F component
// stores one exact byte (0..255), avoiding normalized decoding or another SSBO.
layout(set = 0, binding = 9) uniform sampler2D staticLightOcclusion;
uint occlusionWord(uint index) {
  uint width = frame.occlusion.z;
  uvec4 bytes = uvec4(texelFetch(staticLightOcclusion, ivec2(index % width, index / width), 0));
  return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}
uint staticLightVisibility(uint chart, uint capturedLight) {
  if (chart == 0xffffffffu || capturedLight == 0u)
    return 0u;
  uint stride = frame.occlusion.x;
  uint light = capturedLight - 1u;
  if (light / 16u >= stride || chart >= frame.occlusion.y)
    return 0u;
  uint index = 2u + chart * stride + light / 16u;
  uint width = frame.occlusion.z;
  vec4 bytes = texelFetch(staticLightOcclusion, ivec2(index % width, index / width), 0);
  return (uint(bytes[(light % 16u) / 4u]) >> ((light % 4u) * 2u)) & 3u;
}

#include "overflow-mask.glsl"
