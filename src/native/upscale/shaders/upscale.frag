#version 450
#extension GL_GOOGLE_include_directive : require
#include "../../pathtrace/shaders/display.glsl"
#include "../../postfx/shaders/composite.glsl"
layout(binding=0,rgba16f) readonly uniform image2D upscaledColor;
layout(binding=1,r32f) readonly uniform image2D stableDepth;
// The view effects of what screens and portals show (render resolution: x
// chromatic aberration in pixels of a 1920-wide output, y saturation, z
// contrast, w 1; 0 elsewhere; remixapi_MaterialInfoViewEffectsEXT).
layout(binding=2,rgba16f) readonly uniform image2D viewEffects;
// display and the nits: displayColor (pathtrace/shaders/display.glsl).
// viewEffects: 1 when the image holds this frame's (the path tracer's primary
// surface replacement pass ran).
layout(push_constant) uniform Settings { uint display; float peakNits; float paperWhiteNits; uint viewEffects; } settings;
vec4 viewEffectAt(ivec2 pixel,ivec2 size) {
  ivec2 source=ivec2((vec2(pixel)+.5)*vec2(imageSize(viewEffects))/vec2(size));
  return imageLoad(viewEffects,clamp(source,ivec2(0),imageSize(viewEffects)-1));
}
// Bilinear read of the upscaled colour at a pixel position (centres at .5).
vec3 upscaledAt(vec2 p) {
  ivec2 size=imageSize(upscaledColor);
  vec2 f=p-.5,w=fract(f);ivec2 i=ivec2(floor(f));
  vec3 a=imageLoad(upscaledColor,clamp(i,ivec2(0),size-1)).rgb,b=imageLoad(upscaledColor,clamp(i+ivec2(1,0),ivec2(0),size-1)).rgb,
       c=imageLoad(upscaledColor,clamp(i+ivec2(0,1),ivec2(0),size-1)).rgb,d=imageLoad(upscaledColor,clamp(i+ivec2(1,1),ivec2(0),size-1)).rgb;
  return mix(mix(a,b,w.x),mix(c,d,w.x),w.y);
}
// Source's MonitorScreen: contrast mix(c, c * c, contrast), then saturation
// mix(mean, c, saturation).
vec3 viewColor(vec3 c,vec4 effects) {
  c=mix(c,c*c,effects.z);
  return max(mix(vec3(dot(c,vec3(1.0/3.0))),c,effects.y),vec3(0));
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
  // remixapi_MaterialInfoViewEffectsEXT: the red channel from the right, the
  // blue from the left, where those pixels show such a view too.
  vec4 effects=settings.viewEffects!=0u?viewEffectAt(pixel,imageSize(upscaledColor)):vec4(0);
  if(effects.w>0&&effects.x>0) {
    ivec2 size=imageSize(upscaledColor);
    int shift=max(1,int(round(effects.x*float(size.x)/1920.)));
    ivec2 right=min(pixel+ivec2(shift,0),size-1),left=max(pixel-ivec2(shift,0),ivec2(0));
    if(viewEffectAt(right,size).w>0)color.r=imageLoad(upscaledColor,right).r;
    if(viewEffectAt(left,size).w>0)color.b=imageLoad(upscaledColor,left).b;
  }
  color=postColor(color,gl_FragCoord.xy/vec2(imageSize(upscaledColor)));
  // Saturation and contrast: on the displayed SDR colour, as a game's screen
  // shader sees its camera's image; on the exposed linear colour for HDR.
  const bool effected=effects.w>0&&(effects.y!=1||effects.z!=0);
  if(effected&&(settings.display&1u)!=0u)color=viewColor(color,effects);
  color=displayColor(color,settings.display,settings.peakNits,settings.paperWhiteNits);
  if(effected&&(settings.display&1u)==0u)color=viewColor(clamp(color,0,1),effects);
  result=vec4(color,1);
  gl_FragDepth=imageLoad(stableDepth,pixel).r;
}
