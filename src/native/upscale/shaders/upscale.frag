#version 450
#extension GL_GOOGLE_include_directive : require
#include "../../pathtrace/shaders/display.glsl"
#include "../../postfx/shaders/composite.glsl"
layout(binding=0,rgba16f) readonly uniform image2D upscaledColor;
layout(binding=1,r32f) readonly uniform image2D stableDepth;
// display and the nits: displayColor (pathtrace/shaders/display.glsl).
layout(push_constant) uniform Settings { uint display; float peakNits; float paperWhiteNits; } settings;
// Bilinear read of the upscaled colour at a pixel position (centres at .5).
vec3 upscaledAt(vec2 p) {
  ivec2 size=imageSize(upscaledColor);
  vec2 f=p-.5,w=fract(f);ivec2 i=ivec2(floor(f));
  vec3 a=imageLoad(upscaledColor,clamp(i,ivec2(0),size-1)).rgb,b=imageLoad(upscaledColor,clamp(i+ivec2(1,0),ivec2(0),size-1)).rgb,
       c=imageLoad(upscaledColor,clamp(i+ivec2(0,1),ivec2(0),size-1)).rgb,d=imageLoad(upscaledColor,clamp(i+ivec2(1,1),ivec2(0),size-1)).rgb;
  return mix(mix(a,b,w.x),mix(c,d,w.x),w.y);
}
layout(location=0) out vec4 result;
void main() {
  ivec2 pixel=ivec2(gl_FragCoord.xy);
  vec3 color=imageLoad(upscaledColor,pixel).rgb;
  // Lens chromatic aberration (postfx, rtx.native.pathtrace.chromaticAberration).
  if(lensAberration()>0) {
    vec2 red,blue;
    lensAberrationSamples(gl_FragCoord.xy,vec2(imageSize(upscaledColor)),red,blue);
    color.r=upscaledAt(red).r;color.b=upscaledAt(blue).b;
  }
  color=postColor(color,gl_FragCoord.xy/vec2(imageSize(upscaledColor)));
  color=displayColor(color,settings.display,settings.peakNits,settings.paperWhiteNits);
  result=vec4(color,1);
  gl_FragDepth=imageLoad(stableDepth,pixel).r;
}
