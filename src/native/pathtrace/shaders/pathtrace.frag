#version 450
#extension GL_GOOGLE_include_directive : require
#include "display.glsl"
#include "../../postfx/shaders/composite.glsl"
layout(set=0,binding=4,rgba32f) readonly uniform image2D worldColor;
layout(set=0,binding=5,rgba32ui) readonly uniform uimage2D backgroundTransmission;
// display and the nits: displayColor (display.glsl). falseColor: a debug view
// in false colour, shown without the tone curve.
layout(push_constant) uniform Composite { uint additive; uint falseColor; uint width; uint height; uint display; float peakNits; float paperWhiteNits; } mode;
layout(location=0) out vec4 result;
// Bilinear read of the world colour at a pixel position (centres at .5).
vec3 worldAt(vec2 p) {
  ivec2 size=imageSize(worldColor);
  vec2 f=p-.5,w=fract(f);ivec2 i=ivec2(floor(f));
  vec3 a=imageLoad(worldColor,clamp(i,ivec2(0),size-1)).rgb,b=imageLoad(worldColor,clamp(i+ivec2(1,0),ivec2(0),size-1)).rgb,
       c=imageLoad(worldColor,clamp(i+ivec2(0,1),ivec2(0),size-1)).rgb,d=imageLoad(worldColor,clamp(i+ivec2(1,1),ivec2(0),size-1)).rgb;
  return mix(mix(a,b,w.x),mix(c,d,w.x),w.y);
}
void main() {
  // Diagnostics use unfiltered internal pixels even while FSR remains active.
  ivec2 pixel=ivec2(gl_FragCoord.xy*vec2(imageSize(worldColor))/vec2(mode.width,mode.height));
  vec4 world=imageLoad(worldColor,pixel);
  vec3 color=mode.additive!=0 ? world.rgb : uintBitsToFloat(imageLoad(backgroundTransmission,pixel).rgb);
  if (mode.additive!=0) {
    // Lens chromatic aberration (postfx), in the world image's pixels.
    if(lensAberration()>0) {
      vec2 red,blue,at=gl_FragCoord.xy*vec2(imageSize(worldColor))/vec2(mode.width,mode.height);
      lensAberrationSamples(at,vec2(imageSize(worldColor)),red,blue);
      color.r=worldAt(red).r;color.b=worldAt(blue).b;
    }
    color=postColor(color,gl_FragCoord.xy/vec2(mode.width,mode.height));
    color=mode.falseColor!=0 ? gammaEncodeSafe(color,2.2) : displayColor(color,mode.display,mode.peakNits,mode.paperWhiteNits);
  }
  result=vec4(color,1);
  gl_FragDepth=world.a;
}
