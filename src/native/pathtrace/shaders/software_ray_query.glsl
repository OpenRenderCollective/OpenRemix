// GL_EXT_ray_query over the CPU-built structures of software_bvh.h, for
// devices without ray queries (SOFTWARE_RAY_QUERY). The shaders keep their
// rayQuery*EXT calls: the names below stand in for the extension's, with its
// semantics for triangle geometry (candidates for non-opaque triangles, opaque
// ones committed during traversal, instance masks and flags, ray flags).
#ifndef SOFTWARE_RAY_QUERY_GLSL
#define SOFTWARE_RAY_QUERY_GLSL
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer SoftwareFloats { vec4 f[]; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer SoftwareWords { uvec4 u[]; };

#define gl_RayFlagsNoneEXT 0u
#define gl_RayFlagsOpaqueEXT 1u
#define gl_RayFlagsNoOpaqueEXT 2u
#define gl_RayFlagsTerminateOnFirstHitEXT 4u
#define gl_RayFlagsSkipClosestHitShaderEXT 8u
#define gl_RayFlagsCullBackFacingTrianglesEXT 16u
#define gl_RayFlagsCullFrontFacingTrianglesEXT 32u
#define gl_RayFlagsCullOpaqueEXT 64u
#define gl_RayFlagsCullNoOpaqueEXT 128u
#define gl_RayQueryCommittedIntersectionNoneEXT 0u
#define gl_RayQueryCommittedIntersectionTriangleEXT 1u
#define gl_RayQueryCandidateIntersectionTriangleEXT 0u

// The far children of both levels' open nodes. A deeper path drops its
// farthest subtrees (software_bvh.h Structure::depth). One stack for every
// query of the shader, outside the query: only one query traverses at a time
// (no shader starts a query between another's candidates), and a stack in
// each query costs every one of the megakernel's query sites its own storage.
const int softwareStackSize=48;
uint softwareStack[softwareStackSize];
const uint softwareStepLimit=4096u;
const uint softwareLeaf=0x80000000u,softwareEmpty=0xffffffffu,softwarePop=0xfffffffeu;

struct SoftwareHit {
  float t;
  vec2 barycentrics;
  int primitive,geometry,customIndex;
  uint instance; // word of its instance record
  bool front;
};
struct SoftwareRayQuery {
  uint64_t top,bottom;
  vec3 origin,direction;   // world space
  vec3 o,d,inverse;        // the space being traversed
  float tMin,tMax;         // tMax: the committed hit's, once there is one
  uint flags,mask;
  int depth,base;          // base: the depth the bottom level was entered at
  uint node;               // next reference, or softwarePop
  uint item,itemEnd;       // triangles left in the bottom leaf (words)
  uint instance,instanceEnd; // instances left in the top leaf (words)
  uint topItems,bottomItems; // first leaf-item word of each level
  uint instanceFlags,instanceWord;
  int customIndex;
  bool inBottom,hasCommitted;
  SoftwareHit candidate,committed;
};

vec3 softwareInverse(vec3 d) {
  // A zero component would make the slab test's products NaN.
  return 1./mix(d,vec3(1e-30),lessThan(abs(d),vec3(1e-30)));
}

void softwareInitialize(inout SoftwareRayQuery q,uint64_t scene,uint flags,uint mask,vec3 origin,float tMin,
                        vec3 direction,float tMax) {
  q.top=scene;
  q.origin=q.o=origin;
  q.direction=q.d=direction;
  q.inverse=softwareInverse(direction);
  q.tMin=tMin;
  q.tMax=tMax;
  q.flags=flags;
  q.mask=mask&255u;
  q.depth=q.base=0;
  q.item=q.itemEnd=q.instance=q.instanceEnd=0u;
  q.inBottom=q.hasCommitted=false;
  uvec4 header=SoftwareWords(scene).u[0];
  q.node=header.x==softwareEmpty?softwarePop:header.x;
  q.topItems=header.y;
}

void softwareTerminate(inout SoftwareRayQuery q) {
  q.depth=0;
  q.item=q.itemEnd=q.instance=q.instanceEnd=0u;
  q.inBottom=false;
  q.node=softwarePop;
}

void softwareConfirm(inout SoftwareRayQuery q) {
  q.committed=q.candidate;
  q.hasCommitted=true;
  q.tMax=q.candidate.t;
}

// The next instance of the top leaf: true once its bottom level is entered.
bool softwareEnter(inout SoftwareRayQuery q) {
  const uint word=q.instance;
  q.instance+=8u;
  SoftwareWords words=SoftwareWords(q.top);
  const uvec4 identity=words.u[word+6u];
  if((identity.y&q.mask)==0u)return false;
  SoftwareFloats floats=SoftwareFloats(q.top);
  const vec4 r0=floats.f[word],r1=floats.f[word+1u],r2=floats.f[word+2u];
  const uint64_t bottom=packUint2x32(words.u[word+7u].xy);
  const uvec4 header=SoftwareWords(bottom).u[0];
  if(header.x==softwareEmpty)return false;
  q.bottom=bottom;
  q.o=vec3(dot(r0.xyz,q.origin)+r0.w,dot(r1.xyz,q.origin)+r1.w,dot(r2.xyz,q.origin)+r2.w);
  q.d=vec3(dot(r0.xyz,q.direction),dot(r1.xyz,q.direction),dot(r2.xyz,q.direction));
  q.inverse=softwareInverse(q.d);
  q.node=header.x;
  q.bottomItems=header.y;
  q.customIndex=int(identity.x);
  q.instanceFlags=identity.z;
  q.instanceWord=word;
  q.base=q.depth;
  q.inBottom=true;
  return true;
}

// One triangle of the bottom leaf. 0: no hit, 1: an opaque hit was committed,
// 2: q.candidate is a non-opaque hit.
uint softwareTriangle(inout SoftwareRayQuery q,uint word) {
  SoftwareFloats floats=SoftwareFloats(q.bottom);
  const vec4 v0=floats.f[word],e1=floats.f[word+1u],e2=floats.f[word+2u];
  const vec3 h=cross(q.d,e2.xyz);
  const float a=dot(e1.xyz,h);
  const float f=1./a;
  const vec3 s=q.o-v0.xyz;
  const float u=f*dot(s,h);
  const vec3 c=cross(s,e1.xyz);
  const float v=f*dot(q.d,c);
  if(!(u>=0.&&v>=0.&&u+v<=1.))return 0u;
  const float t=f*dot(e2.xyz,c);
  if(!(t>q.tMin&&t<q.tMax))return 0u;
  // Front faces wind clockwise seen from the origin, in object space;
  // counterclockwise with the instance's flag (2).
  const bool front=(a<0.)!=((q.instanceFlags&2u)!=0u);
  if((q.instanceFlags&1u)==0u&&(q.flags&(front?32u:16u))!=0u)return 0u;
  const uint source=floatBitsToUint(e1.w);
  // Geometry flag, then the instance's force (no) opaque, then the ray's.
  bool opaque=(source&0x80000000u)!=0u;
  if((q.instanceFlags&4u)!=0u)opaque=true;
  else if((q.instanceFlags&8u)!=0u)opaque=false;
  if((q.flags&1u)!=0u)opaque=true;
  else if((q.flags&2u)!=0u)opaque=false;
  if((q.flags&(opaque?64u:128u))!=0u)return 0u;
  q.candidate.t=t;
  q.candidate.barycentrics=vec2(u,v);
  q.candidate.primitive=int(floatBitsToUint(v0.w));
  q.candidate.geometry=int(source&0x7fffffffu);
  q.candidate.customIndex=q.customIndex;
  q.candidate.instance=q.instanceWord;
  q.candidate.front=front;
  if(!opaque)return 2u;
  softwareConfirm(q);
  return 1u;
}

bool softwareProceed(inout SoftwareRayQuery q) {
  // Bounded: a traversal that cannot end (broken structure data) would hold
  // the device in this dispatch.
  for(uint steps=0u;steps<softwareStepLimit;++steps) {
    while(q.item<q.itemEnd) {
      const uint word=q.item;
      q.item+=3u;
      const uint hit=softwareTriangle(q,word);
      if(hit==2u)return true;
      if(hit==1u&&(q.flags&4u)!=0u) {softwareTerminate(q);return false;}
    }
    if(q.node==softwarePop) {
      if(q.inBottom) {
        if(q.depth>q.base)q.node=softwareStack[--q.depth];
        else {
          q.inBottom=false;
          q.o=q.origin;
          q.d=q.direction;
          q.inverse=softwareInverse(q.direction);
        }
      } else if(q.instance<q.instanceEnd)softwareEnter(q);
      else if(q.depth>0)q.node=softwareStack[--q.depth];
      else return false;
      continue;
    }
    const uint reference=q.node;
    if((reference&softwareLeaf)!=0u) {
      const uint count=(reference>>24)&127u,first=reference&0xffffffu;
      if(q.inBottom) {
        q.item=q.bottomItems+first*3u;
        q.itemEnd=q.item+count*3u;
      } else {
        q.instance=q.topItems+first*8u;
        q.instanceEnd=q.instance+count*8u;
      }
      q.node=softwarePop;
      continue;
    }
    SoftwareFloats floats=SoftwareFloats(q.inBottom?q.bottom:q.top);
    const uint word=1u+reference*4u;
    const vec4 leftLow=floats.f[word],leftHigh=floats.f[word+1u];
    const vec4 rightLow=floats.f[word+2u],rightHigh=floats.f[word+3u];
    const vec3 origin=-q.o*q.inverse;
    const vec3 a0=leftLow.xyz*q.inverse+origin,a1=leftHigh.xyz*q.inverse+origin;
    const vec3 b0=rightLow.xyz*q.inverse+origin,b1=rightHigh.xyz*q.inverse+origin;
    const vec3 aNear=min(a0,a1),aFar=max(a0,a1),bNear=min(b0,b1),bFar=max(b0,b1);
    const float aEnter=max(max(aNear.x,aNear.y),max(aNear.z,q.tMin));
    const float aLeave=min(min(aFar.x,aFar.y),min(aFar.z,q.tMax));
    const float bEnter=max(max(bNear.x,bNear.y),max(bNear.z,q.tMin));
    const float bLeave=min(min(bFar.x,bFar.y),min(bFar.z,q.tMax));
    const bool hitA=aEnter<=aLeave,hitB=bEnter<=bLeave;
    const uint left=floatBitsToUint(leftLow.w),right=floatBitsToUint(leftHigh.w);
    if(hitA&&hitB) {
      const bool swap=bEnter<aEnter;
      if(q.depth<softwareStackSize)softwareStack[q.depth++]=swap?left:right;
      q.node=swap?right:left;
    } else if(hitA)q.node=left;
    else if(hitB)q.node=right;
    else q.node=softwarePop;
  }
  softwareTerminate(q);
  return false;
}

mat4x3 softwareObjectToWorld(uint64_t top,uint instance) {
  SoftwareFloats floats=SoftwareFloats(top);
  return transpose(mat3x4(floats.f[instance+3u],floats.f[instance+4u],floats.f[instance+5u]));
}

#define accelerationStructureEXT uint64_t
#define rayQueryEXT SoftwareRayQuery
#define rayQueryInitializeEXT softwareInitialize
#define rayQueryProceedEXT softwareProceed
#define rayQueryTerminateEXT softwareTerminate
#define rayQueryConfirmIntersectionEXT softwareConfirm
// Macros, not functions: a query is too large to pass by value.
#define rayQueryGetIntersectionTypeEXT(q,c) ((c)&&q.hasCommitted?1u:0u)
#define rayQueryGetIntersectionTEXT(q,c) ((c)?q.committed.t:q.candidate.t)
#define rayQueryGetIntersectionBarycentricsEXT(q,c) ((c)?q.committed.barycentrics:q.candidate.barycentrics)
#define rayQueryGetIntersectionPrimitiveIndexEXT(q,c) ((c)?q.committed.primitive:q.candidate.primitive)
#define rayQueryGetIntersectionGeometryIndexEXT(q,c) ((c)?q.committed.geometry:q.candidate.geometry)
#define rayQueryGetIntersectionInstanceCustomIndexEXT(q,c) ((c)?q.committed.customIndex:q.candidate.customIndex)
#define rayQueryGetIntersectionFrontFaceEXT(q,c) ((c)?q.committed.front:q.candidate.front)
#define rayQueryGetIntersectionObjectToWorldEXT(q,c) \
  softwareObjectToWorld(q.top,(c)?q.committed.instance:q.candidate.instance)
#endif
