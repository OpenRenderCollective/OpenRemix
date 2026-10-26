#version 450
// HDR10 presentation (runtime/hdr_output.cpp): the float back buffer, gamma
// 2.2 encoded with 1.0 at paper white, is decoded, converted to BT.2020,
// encoded as ST.2084 PQ with 1.0 at paperWhiteNits and dithered at the
// swapchain's 10 bits.
#extension GL_GOOGLE_include_directive : require
#include "../../pathtrace/shaders/color.glsl"
layout(set=0,binding=0) uniform sampler2D backBuffer;
layout(push_constant) uniform Output { float paperWhiteNits; } hdr;
layout(location=0) out vec4 result;

void main() {
  // Decoded with the sign kept: colours outside BT.709 can land inside BT.2020.
  vec3 linear709 = gammaDecodeSafe(texelFetch(backBuffer, ivec2(gl_FragCoord.xy), 0).rgb, 2.2);
  vec3 linear2020 = max(bt709To2020 * linear709, vec3(0.0));
  vec3 pq = pqEncode(linear2020, hdr.paperWhiteNits);
  float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
  result = vec4(clamp(pq + noise / 1023.0, 0.0, 1.0), 1.0);
}
