struct Vertex { vec4 position; vec4 uv; vec4 color; };
vec3 vertexNormal(Vertex v) { return vec3(v.position.w,v.uv.zw); }
vec3 shadingNormal(vec3 n,vec3 geometric,vec3 wo) {
  if(any(isnan(n))||any(isinf(n))||dot(n,n)<1e-12)return geometric;
  n=normalize(n);
  if(dot(n,geometric)<0)n=-n;
  return dot(n,wo)>1e-4?n:geometric;
}
layout(buffer_reference, std430, buffer_reference_align=16) readonly buffer Vertices { Vertex v[]; };
layout(buffer_reference, std430, buffer_reference_align=4) readonly buffer Indices { uint i[]; };
struct Surface {
  Vertices vertices;
  Indices indices;
  vec4 tint;
  vec4 uv;
  vec4 factor;
  uvec4 state; // texture operations, compare, reference bits, flags
  uvec4 textureState; // texture index
  uint64_t triangleMap;
  Indices traceIndices;
  uint64_t previousVertices;
  uvec2 debugIdentity;
  mat4 previousModel;
  vec4 emission;
  vec4 pbr;
  uvec4 pbrTextures;
  // scene::MaterialDecode: x flags (self-illumination); the rest unused.
  uvec4 material;
};
// Shading reads the AS index order directly. Only debug identity needs the
// original triangle mapping; motion still addresses the original vertex array.
uint originalTriangle(Surface s,uint triangle) {
  return s.triangleMap==0?triangle:Indices(s.triangleMap).i[triangle];
}
layout(set=0,binding=2,std430) readonly buffer Surfaces { Surface surfaces[]; };
layout(set=0,binding=3,std140) uniform Frame {
  mat4 inverseCamera;
  mat4 camera;
  vec4 eye;
  vec4 lightDirection;
  vec4 lightRadiance;
  uvec4 config; // samples, indirect bounces, random sequence, accumulated frames
  uvec4 target; // width, height, structure mask, raster primary visibility
  vec4 jitter; // primary offset; z water reuse; w blue-noise sun/local bits
  vec4 skyRadiance; // linear uniform environment; w reserved
  uint64_t sharcHash, sharcAccumulation, sharcResolved, sharcCounters;
  uvec4 sharcConfig; // bits (1 active, 2 dynamic surfaces, 4 glossy surfaces, 8 no footprint test on queries), capacity, update downscale, debug view
  vec4 sharcGrid; // scene scale, update sample limit (multiple of the cell's mean; 0 none), accumulation frames, stale frames
  vec4 sharcPreviousCamera;
  vec4 sun;
  uint64_t lightNodes, lightEmitters;
  uvec4 localConfig; // active lights, samples, generation low, generation high + adaptive (bit 31) + guide spheres (bit 30)
  vec4 localScale; // radiance multiplier, reach, guiding requested, inverse reach squared
  uint64_t lightGuideRead, lightGuideWrite;
  uvec4 lightGuideConfig; // capacity, node count, inverse cell size (float bits), reserved
  uint64_t torchNodes, torchEmitters, emissiveNodes, emissiveEmitters;
  uvec4 splitLocal;
  uint64_t reuseRead, reuseWrite;
  uvec4 reuseConfig;
  uint64_t indirectResults, blueNoise;
  vec4 firefly; // outgoing local direct / indirect luminance limits; reserved
  uvec4 pathConfig; // adaptive termination, minimum completed bounces, sheet flags (1 traced indirect, 2 local lights; bits 8-12 sky rays at a first surface), secondary ray mask (bits 0-7; bit 8: a view model is traced)
  vec4 domeTransform[3];
  uint64_t visibilityNodes, visibilityLeaves, visibilityTable, movedLights;
  uvec4 visibilityConfig; // table row words, moved light count, reserved
  vec4 visibilityProbe; // world units: x probe offset above the surface, y decal attachment distance; z texture footprint scale (render/output size)
  uint64_t restirCurrent, restirHistoryRead, restirHistoryWrite, restirRemap;
  uvec4 restirConfig; // candidates, spatial samples, temporal M cap, flags (1 initial visibility)
  vec4 restirParams; // spatial radius (pixels), emitter count, reserved
  uint64_t regirRead, regirWrite, regirSlots, regirReserved;
  uvec4 regirConfig; // cell capacity, slots per cell, cells per world unit (float bits), active
  uint64_t orcaTable, lightCookies; // lightCookies: each cookie slot's texture + 1
  uvec4 orcaConfig; // entry capacity, sparse tile size, voxel levels, active
  vec4 orcaParams; // smallest voxel (world units), level growth, minimum samples, maximum relative error (0: any)
  uvec4 cutConfig; // first cut coverage texture + 1 (0: nothing cuts), coverage textures
  uvec4 cutSlots; // four bits per mask group (1..32): its coverage texture (from the first) + 1, or 0
  uint64_t cameraNodes, cameraLeaves; // visibility partition nodes; a bit per leaf, set where the camera sees it
  uvec4 cameraCull; // active camera culling (1 back faces, 2 visibility), probe distance before a surface (float bits)
  uint64_t sparseList, sparseCount; // sparse rendering: selected pixels, their count and dispatch
  uvec4 sparseConfig; // sparse rendering active, threshold levels passed of 256, reserved
  uint64_t volumeGrid; // volumetrics (volume.glsl): the cells of every level
  float animationTime, upAxis; // seconds (wrapped), for scrolling maps; the world's up axis (0 x, 1 y, 2 z)
  uvec4 volumeConfig; // active, cells per side, levels, update interval (frames)
  uvec4 volumeFrame; // update counter, history samples, march steps, reset
  vec4 volumeParams; // smallest cell (world units), march distance, scattered light scale, base height
  vec4 volumeMedium; // scattering albedo; extinction per world unit
  vec4 volumeHeight; // up axis; density falloff per world unit above the base height (0: uniform)
  vec4 volumeEye, volumePrevious; // the windows' centre this frame and last
  vec4 cameraMedium; // remixapi_CameraMediumEXT: transmittance colour, measurement distance (0: in air)
  uint64_t sheetResults; // SheetResults: the sheet lighting pass's transport
  uint64_t skyGuideRead; // sky guiding: last frame's learned sky directions (0: none)
  uint64_t skyGuideWrite;
  uint64_t iesProfiles; // IES profiles (scene/ies.h): 256 floats each
  uvec4 skyGuideConfig; // slot capacity, cells per world unit (float bits), guided share (float bits), active
  mat4 inverseCameraRelative; // camera rays: world = point.xyz / point.w + eye.xyz
} frame;
// Instance mask bit of view models (pathtrace/tlas_policy.h).
const uint viewModelInstanceMask=8u;
// Instance mask bit of reflection-only instances (the player's own body):
// bounce rays see them, camera, shadow and light-visibility rays do not.
const uint reflectionOnlyInstanceMask=16u;
// Set when the camera's primary surface is a view model: the rays of its path
// see view models too (they shadow themselves); every other ray passes them.
bool viewModelPath=false;
// The instance mask of shadow, light-visibility and bounce rays.
uint secondaryMask(){return (frame.pathConfig.w&0xffu)|(viewModelPath?viewModelInstanceMask:0u);}
// Bounce (reflection and indirect) rays: the secondary mask plus reflection-only instances,
// except on a view model's path: the player's own body, drawn around the camera, flickered in
// the weapon's reflections and bounced light onto it.
uint bounceMask(){return secondaryMask()|(viewModelPath?0u:reflectionOnlyInstanceMask);}
// Shadow and light-visibility rays pass paint (each query skips decal
// surfaces): instances of decals alone are left out of their traversal, so a
// ray leaving a painted wall does not visit every quad of paint above it.
const uint decalInstanceMask=32u;
uint shadowMask(){return secondaryMask()&~decalInstanceMask;}
layout(set=0,binding=6) uniform sampler2D textures[4096];
// remixapi_InstanceInfoMaskEXT CUT: the camera does not see a surface where
// its mask group's coverage is set (state.w bits 16-21: the group). Coverage
// is one output-resolution texture per cutting group, zero or one.
// Bit i: coverage texture i is set at uv (the unjittered screen position of
// the camera sample), so the hole's edge is sampled like a geometry edge.
uint cutCoverage(vec2 uv) {
  uint bits=0u;
  for(uint i=0u;i<frame.cutConfig.y;++i) {
    uint index=frame.cutConfig.x-1u+i;
    ivec2 size=textureSize(textures[nonuniformEXT(index)],0);
    if(texelFetch(textures[nonuniformEXT(index)],clamp(ivec2(floor(uv*vec2(size))),ivec2(0),size-1),0).r>0)bits|=1u<<i;
  }
  return bits;
}
// A surface with these state flags (Surface.state.w) is hidden from a camera
// sample with these coverage bits.
bool cutSurface(uint flags,uint bits) {
  uint group=(flags>>16)&63u;
  if(bits==0u||group==0u)return false;
  uint slot=(frame.cutSlots[(group-1u)>>3]>>(((group-1u)&7u)*4u))&15u;
  return slot!=0u&&((bits>>(slot-1u))&1u)!=0u;
}
// remixapi_InstanceInfoCameraCullingEXT (state.w bits 9-10: back faces,
// visibility): what rays along the camera's lines of sight skip of a surface
// that every other ray meets whole.
struct CameraNode { vec4 plane; ivec4 children; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer CameraNodes { CameraNode data[]; };
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer CameraLeaves { uint data[]; };
// Whether p lies in a leaf of the host's partition that the camera's cluster
// does not see (leaves without a cluster are seen).
bool cameraUnseen(vec3 p) {
  CameraNodes nodes=CameraNodes(frame.cameraNodes);
  int index=0;
  for(uint guard=0u;guard<1024u&&index>=0;++guard) {
    CameraNode node=nodes.data[index];
    index=dot(node.plane.xyz,p)-node.plane.w>=0?node.children.x:node.children.y;
  }
  if(index>=0)return false;
  uint leaf=uint(-1-index);
  return ((CameraLeaves(frame.cameraLeaves).data[leaf>>5]>>(leaf&31u))&1u)==0u;
}
// A camera ray (origin, unit direction) meets a surface with these state
// flags at distance t, on its front face or not: hidden from it? The ray is
// located just before the surface, where it is still in the space it came
// through: a line of sight within the visible set never loses its surface.
bool cameraCulled(uint flags,bool front,vec3 origin,vec3 direction,float t) {
  uint cull=(flags>>9)&frame.cameraCull.x;
  if(cull==0u)return false;
  if((cull&1u)!=0u&&!front)return true;
  return (cull&2u)!=0u&&cameraUnseen(origin+direction*max(t-uintBitsToFloat(frame.cameraCull.y),0.));
}
// UV change per output pixel along two footprint axes at the surface being
// shaded (ray differentials, or dFdx/dFdy when rasterized). Zero selects the
// finest mip: visibility tests during traversal keep full-resolution coverage.
vec2 uvGradX=vec2(0),uvGradY=vec2(0);
// A material texture at mesh UV `uv`, filtered over the footprint.
vec4 materialTexel(Surface s,uint index,vec2 uv) {
  return textureGrad(textures[nonuniformEXT(index)],uv*s.uv.xy+s.uv.zw,uvGradX*s.uv.xy,uvGradY*s.uv.xy);
}
vec3 environmentRadiance(vec3 direction) {
  if(frame.skyRadiance.w<=0)return frame.skyRadiance.rgb;
  vec3 d=normalize(vec3(dot(frame.domeTransform[0].xyz,direction),dot(frame.domeTransform[1].xyz,direction),dot(frame.domeTransform[2].xyz,direction)));
  vec2 uv=vec2(atan(d.z,d.x)/(2*3.14159265359)+.5,acos(clamp(d.y,-1,1))/3.14159265359);
  return frame.skyRadiance.rgb*textureLod(textures[nonuniformEXT(uint(frame.skyRadiance.w)-1)],uv,0).rgb;
}
// Perceptual roughness and metalness; albedo is the linear material colour.
// A roughness (map 0) or metallic (1) texel's value (frame_tables.h
// materialFields: remixapi_MaterialPbrRemapEXT).
float materialMapValue(Surface s,uint map,vec4 texel) {
  uint bits=s.material.w>>(8*map);
  if((bits&128u)==0)return texel.r;
  float x=texel[bits&3u];
  if((bits&4u)!=0)x=1-x;
  x=pow(max(x,1e-6),float((s.material.w>>(16+8*map))&255u)/16);
  float v=s.pbr[map]+uintBitsToFloat(s.material[1+map])*x;
  return (bits&8u)!=0?sqrt(sqrt(2/(max(v,0)+2))):v;
}
vec2 materialParameters(Surface s,vec2 uv,inout vec3 albedo) {
  float metallic=s.pbrTextures.y==0?s.pbr.y:materialMapValue(s,1,materialTexel(s,s.pbrTextures.y-1,uv));
  if(s.pbrTextures.x==0)return clamp(vec2(s.pbr.x,metallic),0,1);
  return clamp(vec2(materialMapValue(s,0,materialTexel(s,s.pbrTextures.x-1,uv)),metallic),0,1);
}
// As materialTexel, at UVs scaled by k (the footprint scales with them).
vec4 materialTexelScaled(Surface s,uint index,vec2 uv,float k) {
  return textureGrad(textures[nonuniformEXT(index)],uv*s.uv.xy+s.uv.zw,uvGradX*s.uv.xy*k,uvGradY*s.uv.xy*k);
}
mat3 materialBasis(vec3 n,vec3 e1,vec3 e2,vec2 u1,vec2 u2) {
  float det=u1.x*u2.y-u1.y*u2.x;
  vec3 t=abs(det)>1e-10?(e1*u2.y-e2*u1.y)/det:vec3(0);
  t-=n*dot(n,t);
  if(dot(t,t)<1e-12)t=cross(abs(n.z)<.999?vec3(0,0,1):vec3(0,1,0),n);
  t=normalize(t);
  vec3 b=cross(n,t);
  if(abs(det)>1e-10&&dot(b,(e2*u1.x-e1*u2.x)/det)<0)b=-b;
  return mat3(t,b,n);
}
vec3 materialNormal(Surface s,vec2 uv,mat3 basis,vec3 wo) {
  if(s.pbrTextures.z==0)return basis[2];
  vec3 local;
  if((s.pbrTextures.w&16)!=0) {
    // Source's water (normal encoding 5; frame_tables.h materialFields): the
    // map at the UVs its $bumptransform scrolls to, and with multi-texture
    // layers the Water shader's two more samples, averaged.
    uint map=s.pbrTextures.z-1;
    float t=frame.animationTime;
    local=materialTexel(s,map,uv+unpackHalf2x16(s.material.y)*t).xyz;
    if((s.pbrTextures.w&32)!=0)
      local=(local+materialTexelScaled(s,map,vec2(uv.x+uv.y,uv.y-uv.x)*.1+unpackHalf2x16(s.material.z)*t,.14142136).xyz+
             materialTexelScaled(s,map,uv.yx*.45+unpackHalf2x16(s.material.w)*t,.45).xyz)/3;
    local=local*2-1;
  } else local=materialTexel(s,s.pbrTextures.z-1,uv).xyz;
  // Source's $ssbump: weights of its bump basis directions (bumpBasis[3]).
  if(s.pbrTextures.w==8)local=mat3(.81649658,0,.57735027,-.40824829,.70710678,.57735027,-.40824829,-.70710678,.57735027)*local;
  else if((s.pbrTextures.w&16)==0)local=local*2-1;
  if((s.pbrTextures.w&1)!=0)local.z=sqrt(max(0,1-dot(local.xy,local.xy)));
  if(s.pbrTextures.w==2) { local.xy=vec2(local.x+local.y,local.x-local.y)*.5; local.z=1-abs(local.x)-abs(local.y); }
  if((s.pbrTextures.w&4)!=0)local.y=-local.y;
  if(dot(local,local)<1e-8)return basis[2];
  vec3 n=normalize(basis*vec3(local.xy,max(local.z,.001)));
  // A backfacing mapped normal cannot define a reflective hemisphere for wo.
  return dot(n,wo)>1e-4?n:basis[2];
}
vec2 encodeGeometricNormal(vec3 n) {
  n/=abs(n.x)+abs(n.y)+abs(n.z);
  return n.z>=0?n.xy:(1-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
}
vec3 decodeGeometricNormal(vec2 e) {
  vec3 n=vec3(e,1-abs(e.x)-abs(e.y));
  if(n.z<0)n.xy=(1-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
  return normalize(n);
}
vec3 materialEmission(Surface s,vec2 uv) {
  if(!any(greaterThan(s.emission.rgb,vec3(0))))return vec3(0);
  return s.emission.rgb*(s.textureState.z==0?vec3(1):
      materialTexel(s,s.textureState.z-1,uv).rgb);
}
vec3 linearColor(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
vec4 arg(uint index, vec4 texel, vec4 vertex, vec4 factor) {
  return index == 1 ? texel : index == 2 ? vertex : index == 3 ? factor : vec4(1);
}
vec4 combine(uint op, vec4 a, vec4 b) { return op == 1 ? a : op == 2 ? b : op == 6 ? min(a+b,vec4(1)) : min(a*b*(op==4||op==7?2:op==5?4:1),vec4(1)); }
// Colour operation 7: the second argument tints the first where the first's
// alpha says so (Source's $blendtintbybasealpha: a player model's coloured parts).
vec4 combineColor(uint op, vec4 a, vec4 b) { return op == 7 ? vec4(mix(a.rgb, min(a.rgb*b.rgb,vec3(1)), a.a), a.a) : combine(op, a, b); }
// Captured alpha blending represents coverage: uncovered texels transmit the
// background unchanged, regardless of their RGB. Only authored thin-sheet
// materials (constant opacity or glass) retain the colored-transmission model.
vec3 transmissionColor(vec3 color, bool tinted) { return tinted ? color : vec3(1); }
// What a sheet met at distance t along a light or shadow ray lets through. A
// translucent body (frame_tables.h materialFields: refractive index and
// measurement distance in pbr.w) passes its colour to the power of t over the
// measurement distance (the ray's length inside it, as from a surface under
// water), less its Fresnel reflection at normal incidence; any other sheet
// its tinted coverage rule.
vec3 sheetTransmission(Surface s, vec4 color, float t) {
  if ((s.state.w&8)!=0 && s.pbr.w!=0) {
    vec2 body=unpackHalf2x16(floatBitsToUint(s.pbr.w));
    float f0=(body.x-1)/(body.x+1);
    return (1-f0*f0)*pow(clamp(color.rgb,vec3(1e-4),vec3(1)),vec3(t/max(body.y,1e-3)));
  }
  return transmissionColor(color.rgb,(s.state.w&32)!=0)*(1-color.a);
}
// Candidate intersections need opacity only. In particular, do not linearize
// vertex RGB or evaluate the color combiner before knowing the closest hit.
// Bit 0: the combiner reads the texture; bit 1: it reads vertex alpha.
uint alphaOperands(Surface s) {
  uint op=s.state.x,first=(op>>8)&3,second=(op>>10)&3,combineOp=((op>>6)&3)|((op>>11)&4);
  uint a=combineOp==2?0u:first,b=combineOp==1?0u:second;
  return uint(a==1||b==1)|uint(a==2||b==2)<<1;
}
// materialColor's alpha from the texture's alpha (texel) and the vertex's.
float materialAlphaOf(Surface s, float texel, float vertex) {
  if ((s.state.w&1)!=0) return 1;
  uint op=s.state.x;
  uint first=(op>>8)&3,second=(op>>10)&3;
  float a=first==1?texel:first==2?vertex:first==3?s.factor.a:1;
  float b=second==1?texel:second==2?vertex:second==3?s.factor.a:1;
  uint combineOp=((op>>6)&3)|((op>>11)&4);
  float alpha=(combineOp==1?a:combineOp==2?b:combineOp==6?min(a+b,1):min(a*b*(combineOp==4||combineOp==7?2:combineOp==5?4:1),1))*s.tint.a;
  if ((s.state.w&8)!=0) alpha*=.2;
  return clamp(alpha,0,1);
}
float materialAlpha(Surface s, vec2 uv, float vertex, bool sampled) {
  if ((s.state.w&1)!=0) return 1;
  float texel=sampled?textureLod(textures[nonuniformEXT(s.textureState.x)],uv*s.uv.xy+s.uv.zw,0).a:1;
  return materialAlphaOf(s,texel,vertex);
}
vec4 materialColor(Surface s, vec4 texel, vec4 vertex) {
  vertex.rgb = linearColor(vertex.rgb);
  uint op = s.state.x;
  vec4 color = combineColor((op&3)|((op>>10)&4), arg((op>>2)&3,texel,vertex,s.factor), arg((op>>4)&3,texel,vertex,s.factor));
  vec4 alpha = combine(((op>>6)&3)|((op>>11)&4), arg((op>>8)&3,texel,vertex,s.factor), arg((op>>10)&3,texel,vertex,s.factor));
  vec4 result = vec4(color.rgb, alpha.a)*s.tint;
  if ((s.state.w&8)!=0) result.a*=0.2; // Basic thin tinted-sheet approximation.
  if ((s.state.w&1)!=0) result.a=1;
  return clamp(result,0,1);
}
vec4 materialColor(Surface s, vec2 uv, vec4 vertex) {
  return materialColor(s, materialTexel(s, s.textureState.x, uv), vertex);
}
// The colour a surface reflects, what it emits (when `emissive`) and `self`,
// the part of its colour that emits instead of reflecting
// (remixapi_MaterialInfoSelfIlluminationEXT: material.x bit 5, the mask in
// bits 6-7, its scale in emission.w): colour = reflecting + self.
vec4 materialSurface(Surface s, vec2 uv, vec4 vertex, bool emissive, out vec3 emission, out vec3 self) {
  vec4 texel = materialTexel(s, s.textureState.x, uv);
  vec4 color = materialColor(s, texel, vertex);
  self = vec3(0);
  if ((s.material.x&32u)==0) { emission = emissive ? materialEmission(s, uv) : vec3(0); return color; }
  uint mask = (s.material.x>>6)&3u;
  vec4 map = s.textureState.z==0 ? vec4(1) : materialTexel(s, s.textureState.z-1, uv);
  vec3 m = (mask==2u ? vec3(map.a) : map.rgb) * (mask==1u ? texel.a : 1.) * s.emission.w;
  emission = emissive ? s.emission.rgb*color.rgb*m : vec3(0);
  self = color.rgb*clamp((m.r+m.g+m.b)/3, 0, 1);
  color.rgb -= self;
  return color;
}
// The G-buffer keeps the whole colour and, in the emission's alpha, the share
// of it that is self-illuminated, negated (positive values are debug identities).
float selfShare(vec3 reflecting, vec3 self) {
  float whole = reflecting.r+reflecting.g+reflecting.b+self.r+self.g+self.b;
  return whole>0 ? clamp((self.r+self.g+self.b)/whole, 0, 1) : 0;
}
bool decalSurface(Surface s) { return (s.state.w&64u)!=0; }
bool coverageBlended(Surface s) { return (s.state.w&34u)==2u; }
bool blendedAtAlpha(Surface s,float a) { return (s.state.w&2u)!=0&&a<1; }
bool alphaPass(Surface s, float a) {
  float ref=uintBitsToFloat(s.state.z); uint op=s.state.y;
  return op==7 || (op==1&&a<ref) || (op==2&&a==ref) || (op==3&&a<=ref) ||
    (op==4&&a>ref) || (op==5&&a!=ref) || (op==6&&a>=ref);
}

// Zero-coverage texels do not represent an interaction. A tinted sheet still
// transmits its color at zero opacity and must remain in the layer traversal.
bool visibleAlpha(Surface s,float a) {
  return alphaPass(s,a)&&(!coverageBlended(s)||a>0);
}
