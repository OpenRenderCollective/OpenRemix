#extension GL_GOOGLE_include_directive : require
#define NRD_GLSL 1
#include "NRD.hlsli"
layout(set=1,binding=0,rgba16f) uniform image2D sh0;
layout(set=1,binding=1,rgba16f) uniform image2D sh1;
layout(set=1,binding=2,rgb10_a2) uniform image2D guideNormal;
layout(set=1,binding=3,r32f) uniform image2D guideDepth;
layout(set=1,binding=4,rgba32f) uniform image2D guideMotion;
layout(set=1,binding=5,rgba16f) uniform image2D guideAlbedo;
layout(set=1,binding=6,rgba16f) uniform image2D directLight;
layout(set=1,binding=7,rgba16f) uniform image2D transparentLight;
layout(set=1,binding=8,rgba16f) uniform image2D transmittance;
layout(set=1,binding=9,rgba16f) uniform image2D outSh0;
layout(set=1,binding=10,rgba16f) uniform image2D outSh1;
layout(set=1,binding=11,r32f) uniform image2D confidence;
layout(set=1,binding=12,rgba16f) uniform image2D gradientA;
layout(set=1,binding=13,rgba16f) uniform image2D gradientB;
layout(set=1,binding=14,rgba16f) uniform image2D history0;
layout(set=1,binding=15,rgba16f) uniform image2D history1;
layout(set=1,binding=16,r32f) uniform image2D previousDepth;
layout(set=1,binding=17,rgb10_a2) uniform image2D previousNormal;
layout(set=1,binding=18,rgba16f) uniform image2D previousLighting;
layout(set=1,binding=19,rgba16f) uniform image2D lighting;
layout(set=1,binding=20,rgba16f) uniform image2D composed;
layout(set=1,binding=21,rgba16f) uniform image2D validation;
layout(set=1,binding=22,rgba32f) uniform image2D finalWorld;
layout(set=1,binding=23,rgba32f) uniform image2D skyColor;
#ifdef SIGMA_PASS
layout(set=1,binding=26,r16f) uniform image2D shadowPenumbra;
layout(set=1,binding=27,rgba8) uniform image2D shadowInput;
layout(set=1,binding=28,rgba8) uniform image2D shadowOutput;
#endif
#ifdef LOCAL_PASS
layout(set=1,binding=29,rgba16f) uniform image2D localInput;
layout(set=1,binding=30,rgba16f) uniform image2D localOutput;
#endif
#ifdef SPLIT_LOCAL_PASS
layout(set=1,binding=31,rgba16f) uniform image2D emissiveInput;
layout(set=1,binding=32,rgba16f) uniform image2D emissiveOutput;
#endif
layout(set=1,binding=33,rgba16f) uniform image2D pbrSpecularInput;
layout(set=1,binding=34,rgba16f) uniform image2D pbrDiffuseOutput;
layout(set=1,binding=35,rgba16f) uniform image2D pbrSpecularOutput;
layout(set=1,binding=36,rgba16f) uniform image2D pbrSpecularFactor;
layout(set=1,binding=37,rgba16f) uniform image2D rrDiffuse;
layout(set=1,binding=38,rgba16f) uniform image2D rrSpecular;
layout(set=1,binding=39,rgba16f) uniform image2D rrNormal;
layout(set=1,binding=40,r32f) uniform image2D rrHitDistance;
layout(set=1,binding=41,rgb10_a2) uniform image2D frontNormal;
layout(set=1,binding=42,r32f) uniform image2D frontDepth;
layout(set=1,binding=43,rgba32f) uniform image2D frontMotion;
layout(set=1,binding=44,rgba16f) uniform image2D frontAlbedo;
layout(set=1,binding=45,rgba16f) uniform image2D frontSpecularFactor;
layout(set=1,binding=46,rgba16f) uniform image2D frontDiffuse;
layout(set=1,binding=47,rgba16f) uniform image2D frontSpecular;
layout(set=1,binding=48,rgba16f) uniform image2D frontDiffuseOutput;
layout(set=1,binding=49,rgba16f) uniform image2D frontSpecularOutput;
layout(set=1,binding=50,r32f) uniform image2D frontConfidence;
layout(set=1,binding=51,r32f) uniform image2D localConfidence;
// Primary surface replacement (PSR) through a mirror: the mirror plane
// (octahedral normal, distance from the origin) and its reflectance, applied
// to the replaced surface's composed lighting. w=0 marks pixels without PSR.
layout(set=1,binding=52,rgba32ui) uniform uimage2D psrMirror;
// SH directions of the PBR pass (REBLUR_DIFFUSE_SPECULAR_SH): its diffuse SH
// input is sh0/sh1, its specular SH0 is pbrSpecularInput, and its outputs'
// SH0 are pbrDiffuseOutput/pbrSpecularOutput. Full size only with REBLUR.
layout(set=1,binding=53,rgba16f) uniform image2D pbrSpecularSh1;
layout(set=1,binding=54,rgba16f) uniform image2D pbrDiffuseOutputSh1;
layout(set=1,binding=55,rgba16f) uniform image2D pbrSpecularOutputSh1;
// The gradient pass's tiles on surfaces that move on their own (ping-pong
// with the gradient images): signed indirect difference to last frame, its
// level (> 0: a followed tile), relative change of the unshadowed local
// light, and the pose-change estimate of the indirect change.
layout(set=1,binding=56,rgba16f) uniform image2D movingA;
layout(set=1,binding=57,rgba16f) uniform image2D movingB;
// The local-light passes' motion vectors (guideMotion, pointed off screen
// where an occluder moved: localOcclusionConfidence).
layout(set=1,binding=58,rgba32f) uniform image2D localMotion;
layout(set=1,binding=25,std140) uniform TemporalFrame {
  mat4 view;
  mat4 previousView;
  mat4 previousCamera;
  mat4 previousInverseCamera;
  mat4 inverseCamera;
  vec4 jitter;
  vec4 hitDistance;
  uvec4 flags;
  uvec4 target;
  vec4 timing;
  vec4 sun;
  uvec4 shadow;
  uvec4 local;
  uvec4 translucency;
  uvec4 families;
  vec4 motion; // moving surfaces: ramp start, end (pixels per frame), indirect floor, local floor
  vec4 motionGradient; // neighbour floor, gradients follow moving surfaces, tolerated change per frame, 1 / lighting length scale
  vec4 motionNoise; // measured indirect change below this is sampling noise; native TAA blend rate under subpixel motion
} temporal;
// History confidence of a surface that moved `pixels` on its own this frame
// (motion guide w - 1), never below `least`.
float movingConfidence(float pixels,float least) {
  return max(least,1-smoothstep(temporal.motion.x,max(temporal.motion.y,temporal.motion.x+1e-3),pixels));
}
vec3 safeRadiance(vec3 v) { return any(isnan(v))||any(isinf(v))?vec3(0):clamp(v,vec3(0),vec3(65000)); }
vec2 encodeNormal(vec3 n) {
  n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-10);
  return n.z>=0?n.xy:(1-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
}
vec3 decodeNormal(vec2 e) {
  vec3 n=vec3(e,1-abs(e.x)-abs(e.y));float t=clamp(-n.z,0,1);
  n.xy+=mix(vec2(t),vec2(-t),greaterThanEqual(n.xy,vec2(0)));return normalize(n);
}
struct Mirror { vec3 n; float d; vec3 reflectance; bool replaced; };
Mirror loadMirror(ivec2 p) {
  Mirror m=Mirror(vec3(0,0,1),0,vec3(1),false);
  if(temporal.translucency.z==0)return m;
  uvec4 v=imageLoad(psrMirror,p);
  if(v.w==0u)return m;
  m.n=decodeNormal(unpackSnorm2x16(v.x));m.d=uintBitsToFloat(v.y);
  m.reflectance=vec3(unpackHalf2x16(v.z),unpackHalf2x16(v.w).x);m.replaced=true;
  return m;
}
uvec4 packMirror(vec3 n,vec3 point,vec3 reflectance) {
  return uvec4(packSnorm2x16(encodeNormal(n)),floatBitsToUint(dot(n,point)),
               packHalf2x16(reflectance.rg),packHalf2x16(vec2(reflectance.b,1)));
}
// Reflections through the mirror plane: the virtual (mirrored) position and
// direction of a surface seen in it.
vec3 mirrorPoint(Mirror m,vec3 p) { return p-2*(dot(m.n,p)-m.d)*m.n; }
vec3 mirrorVector(Mirror m,vec3 v) { return v-2*dot(m.n,v)*m.n; }
