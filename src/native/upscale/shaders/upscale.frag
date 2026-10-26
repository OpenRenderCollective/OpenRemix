#version 450
#extension GL_GOOGLE_include_directive : require
#include "../../pathtrace/shaders/display.glsl"
#include "../../postfx/shaders/composite.glsl"
layout(binding=0,rgba16f) readonly uniform image2D upscaledColor;
layout(binding=1,r32f) readonly uniform image2D stableDepth;
// display and the nits: displayColor (pathtrace/shaders/display.glsl).
layout(push_constant) uniform Settings { uint display; float peakNits; float paperWhiteNits; } settings;
layout(location=0) out vec4 result;
void main() {
  ivec2 pixel=ivec2(gl_FragCoord.xy);
  vec3 color=imageLoad(upscaledColor,pixel).rgb;
  color=postColor(color,gl_FragCoord.xy/vec2(imageSize(upscaledColor)));
  color=displayColor(color,settings.display,settings.peakNits,settings.paperWhiteNits);
  result=vec4(color,1);
  gl_FragDepth=imageLoad(stableDepth,pixel).r;
}
