#version 460
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
layout(push_constant) uniform Draw {
  mat4 model;
  vec2 jitter;
  uint surfaceIndex;
  uint stableDepth;
  vec4 reverseDepth;
} draw;
layout(location=0) in vec3 position;
layout(location=1) flat in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 color;
layout(location=4) in vec3 previousPosition;
layout(location=5) flat in float triangleIdentity;
layout(location=6) flat in vec3 tangent;
layout(location=7) flat in vec3 bitangent;
layout(location=8) in vec3 authoredNormal;
#ifdef STABLE_DEPTH
layout(location=0) out float outDepth;
#else
layout(location=0) out vec4 outPosition;
layout(location=1) out vec4 outNormal;
layout(location=2) out vec4 outAlbedo;
layout(location=3) out vec4 outPrevious;
layout(location=4) out vec4 outEmission;
layout(location=5) out vec4 outPbr;
#endif
// The fragment's reverse depth as the world camera maps it (gl_FragCoord.z,
// except for a view model, whose depth test runs on its own scale; nearer than
// the near plane is clamped to it).
float worldDepth() {
  if((draw.stableDepth&2u)==0u)return gl_FragCoord.z;
  return clamp(dot(draw.reverseDepth,vec4(position,1))/(frame.camera*vec4(position,1)).w,0,1);
}
void main() {
  Surface s=surfaces[draw.surfaceIndex];
  // A CUT instance is not seen where its mask group covers. The G-buffer is
  // sampled at the jittered position; stable depth at the output pixel.
  if(frame.cutConfig.x!=0u&&(s.state.w&0x3f0000u)!=0u) {
#ifdef STABLE_DEPTH
    vec2 screen=gl_FragCoord.xy/vec2(textureSize(textures[nonuniformEXT(frame.cutConfig.x-1u)],0));
#else
    vec2 screen=(gl_FragCoord.xy+draw.jitter)/vec2(frame.target.xy);
#endif
    if(cutSurface(s.state.w,cutCoverage(screen)))discard;
  }
  // Match world-space ray facing independently of projection handedness.
  // The geometric normal includes mirrored instance transforms.
  vec4 projected=frame.camera*vec4(position,1);
  vec4 nearPoint=frame.inverseCamera*vec4(projected.xy/projected.w,0,1);
  vec3 direction=position-nearPoint.xyz/nearPoint.w;
  if((s.state.w&4)==0&&dot(normal,direction)<=0)discard;
  // What the camera does not see of the instance (back faces, parts in
  // clusters outside its visible set), as cameraHit rejects them.
  if(frame.cameraCull.x!=0u) {
    float distance=length(direction);
    if(cameraCulled(s.state.w,dot(normal,direction)>0,nearPoint.xyz/nearPoint.w,direction/distance,distance))discard;
  }
  // Footprint per output pixel (visibilityProbe.z: render/output size).
  uvGradX=dFdx(uv)*frame.visibilityProbe.z;
  uvGradY=dFdy(uv)*frame.visibilityProbe.z;
#ifdef STABLE_DEPTH
  // Blended sheets do not occlude raster particles or coplanar feedback.
  if((s.state.w&2)!=0)discard;
  // Coverage only, as the G-buffer's alpha test: the texture's alpha, read
  // only where the test and the combiner use it (no colour, no emission).
  // Every opaque draw is shaded again here at output resolution.
  if(s.state.y!=7u) {
    float texel=(alphaOperands(s)&1u)!=0?materialTexel(s,s.textureState.x,uv).a:1;
    if(!visibleAlpha(s,materialAlphaOf(s,texel,color.a)))discard;
  }
  outDepth=1-worldDepth();
#else
  vec3 emission,self;
  vec4 albedo=materialSurface(s,uv,color,true,emission,self);
  if(!visibleAlpha(s,albedo.a))discard;
  vec3 n=normalize(normal);
  if(dot(n,direction)>0)n=-n;
  outPosition=vec4(position,1-worldDepth());
  // Zero is background, one opaque/cutout, two coverage, three a tinted sheet,
  // four a decal; plus 8 on a view model (stableDepth bit 1), plus 16 with a
  // thin film (its ray is traced again: pathtrace.comp rasterSurface).
  vec3 smoothNormal=shadingNormal(authoredNormal,n,normalize(-direction));
  vec3 t=tangent-smoothNormal*dot(smoothNormal,tangent);
  if(dot(t,t)<1e-12)t=cross(abs(smoothNormal.z)<.999?vec3(0,0,1):vec3(0,1,0),smoothNormal);
  t=normalize(t);
  vec3 b=cross(smoothNormal,t);
  if(dot(b,bitangent)<0)b=-b;
  outNormal=vec4(materialNormal(s,uv,mat3(t,b,smoothNormal),normalize(-direction)),(decalSurface(s)?4:blendedAtAlpha(s,albedo.a)?((s.state.w&32)!=0?3:2):1)+((draw.stableDepth&2u)!=0u?8:0)+(filmed(s)?16:0));
  vec2 rm=materialParameters(s,uv,albedo.rgb);
  outPbr=vec4(s.pbr.z!=0?rm.x:-1,rm.y,encodeGeometricNormal(n));
  outAlbedo=vec4(albedo.rgb+self,albedo.a);
  outPrevious=vec4(previousPosition,s.previousVertices!=0?1:0);
  outEmission=vec4(emission,triangleIdentity>0?triangleIdentity:-selfShare(albedo.rgb,self));
#endif
}
