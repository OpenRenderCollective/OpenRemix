// Analytic sphere next-event estimation, RT Gems I chapter 18. The selection
// PDF and solid-angle PDF both belong in the estimator. No light-count ray loop.
struct LightEmitter { vec4 positionRadius; vec4 radiance; vec4 corners[3]; vec4 directionCone; vec4 shaping; };
struct LightNode { vec4 minimumPower; vec4 maximum; uvec4 links; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer LightEmitters { LightEmitter data[]; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer LightNodes { LightNode data[]; };
float lightRandom(inout uint state) {
  state=hash(state+0x9e3779b9u);return float(state>>8)*(1.0/16777216.0);
}
// A leaf's emitters are consecutive (links: leaf rank, emitter count, first
// emitter, 1): one is chosen by its share of the leaf's power (shaping.z: the
// running share through it, shaping.w: its own), which the caller's PDF takes.
// A leaf of one draws no random number.
uint leafEmitter(LightEmitters emitters,uvec4 links,inout uint state,out float share) {
  share=1;
  uint count=max(links.y,1u);
  if(count==1)return links.z;
  float u=lightRandom(state);
  uint low=0,high=count-1;
  while(low<high) {
    uint middle=(low+high)/2;
    if(u<emitters.data[links.z+middle].shaping.z)high=middle;else low=middle+1;
  }
  share=emitters.data[links.z+low].shaping.w;
  return links.z+low;
}
float localFalloff(float distance2) {
  float reach=frame.localScale.y;
  // Preserve the inner 75%; a C1 fade reaches exactly zero at the bound.
  float t=clamp((distance2*frame.localScale.w-.5625)*(16.0/7.0),0,1);
  return reach<=0?1:(1-t)*(1-t)*(1+2*t);
}
float lightWeight(LightNode node,vec3 p,vec3 n) {
  vec3 halfSize=.5*(node.maximum.xyz-node.minimumPower.xyz);
  vec3 delta=.5*(node.maximum.xyz+node.minimumPower.xyz)-p;
  float radius2=dot(halfSize,halfSize);
  // Cull by the box's farthest reach along the normal, not its enclosing
  // sphere's: a wide cluster behind the receiving plane (lit windows of
  // buildings beyond a wall) otherwise draws samples that all end at zero.
  // (A select, not an early return: the tree walk interleaves two samples'
  // weights, which a branch per child would serialize.)
  bool facing=dot(delta,n)+dot(halfSize,abs(n))>0;
  vec3 nearest=max(abs(delta)-halfSize,vec3(0));
  float cosine=min(1,(dot(delta,n)+sqrt(radius2))*inversesqrt(max(max(dot(delta,delta),radius2),1e-12)));
  // Nearest-box distance keeps nearby lights competitive, but becomes zero
  // anywhere inside a cluster. Do not concentrate the entire cluster's flux
  // into its smallest emitter-sized distance: that starves sibling clusters
  // and produces enormous inverse-PDF samples. A tenth of the cluster radius
  // regularizes this importance estimate; emitter radiance is unchanged.
  // Alone, the nearest distance also credits a tall box's whole flux to its
  // closest corner: a cluster of street lamps whose box reaches down to a
  // tunnel ceiling starved the tunnel's own light. The product with the
  // center distance keeps nearby boxes ahead of far ones.
  float distance2=max(sqrt(dot(nearest,nearest)*dot(delta,delta)),.01*radius2);
  float weight=localFalloff(dot(nearest,nearest))*node.minimumPower.w*cosine/max(max(distance2,node.maximum.w),1e-12);
  return facing?weight:0;
}
// Potentially visible lights (remixapi_SetVisibilityPartitionEXT): the
// shading point's cluster comes from the host's BSP; a table row per cluster
// marks the combined light-tree nodes holding a potentially visible light.
// Excluded subtrees get zero selection weight everywhere the tree is
// traversed, so every PDF stays consistent. Lights outside the PVS cannot
// reach the point, so no energy is lost.
struct VisibilityNode { vec4 plane; ivec4 children; };
layout(buffer_reference,std430,buffer_reference_align=16) readonly buffer VisibilityNodes { VisibilityNode data[]; };
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer VisibilityInts { int data[]; };
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer VisibilityWords { uint data[]; };
int visibilityCluster(vec3 p) {
  VisibilityNodes nodes=VisibilityNodes(frame.visibilityNodes);
  int index=0;
  for(uint guard=0;guard<256u&&index>=0;++guard) {
    VisibilityNode node=nodes.data[index];
    index=dot(node.plane.xyz,p)-node.plane.w>=0?node.children.x:node.children.y;
  }
  return index<0?VisibilityInts(frame.visibilityLeaves).data[-1-index]:-1;
}
// The shading point's cluster, evaluated once per point. Only primary
// (opaque, guided) light sampling culls: the lookup at every bounce vertex
// cost more than its variance reduction there. Each estimator call uses one
// consistent set of weights either way (localLighting sets the flag).
vec3 visibilityPoint=vec3(3.4e38);int visibilityRow=-1;bool visibilityCulling=false;
bool lightNodeVisible(LightNodes nodes,uint index,vec3 p,vec3 n) {
  if(!visibilityCulling||frame.visibilityTable==0||uint64_t(nodes)!=frame.lightNodes)return true;
  // The tree's dynamic part (lights that come and go) is not in the table.
  if(index>=frame.visibilityConfig.z)return true;
  if(p!=visibilityPoint) {
    visibilityPoint=p;
    visibilityRow=visibilityCluster(p+n*frame.visibilityProbe.x);
  }
  if(visibilityRow<0)return true;
  uint word=uint(visibilityRow)*frame.visibilityConfig.x+(index>>5);
  return (VisibilityWords(frame.visibilityTable).data[word]>>(index&31u)&1u)!=0;
}
float nodeWeight(LightNodes nodes,uint index,vec3 p,vec3 n) {
  return lightNodeVisible(nodes,index,p,n)?lightWeight(nodes.data[index],p,n):0;
}
// lightNodeVisible as a factor, without branches, for the interleaved walk.
// table: culling applies (visibilityTableApplies). Without a table, a word of
// the node buffer stands in for the load and the result is 1.
float lightNodeVisibility(LightNodes nodes,uint index,bool table) {
  uint word=table?uint(visibilityRow)*frame.visibilityConfig.x+(index>>5):0u;
  uint bits=VisibilityWords(table?frame.visibilityTable:uint64_t(nodes)).data[word];
  return !table||index>=frame.visibilityConfig.z||(bits>>(index&31u)&1u)!=0?1.0:0.0;
}
// Whether lightNodeVisible consults the table at p; brings visibilityRow up
// to date for lightNodeVisibility.
bool visibilityTableApplies(LightNodes nodes,vec3 p,vec3 n) {
  if(!visibilityCulling||frame.visibilityTable==0||uint64_t(nodes)!=frame.lightNodes||frame.visibilityConfig.z==0)return false;
  lightNodeVisible(nodes,0,p,n);
  return visibilityRow>=0;
}
// A fixed topological cut keeps all nearby clusters in the budget estimate.
// Following only the largest-weight child can abruptly choose an unrelated
// leaf when animated emission or receiver motion changes a nearly tied split.
// The resulting sample/candidate discontinuities look like flickering quads.
// Sum at most sixteen cluster weights (four levels), with no visibility rays.
// Saturation is safe because all terms are nonnegative and .06 already requests
// the maximum adaptive budget. The cut depends on topology, not receiver data.
float localImportance(LightNodes nodes,Hit hit) {
  uint pending[5],levels[5];
  pending[0]=0;levels[0]=0;
  uint count=1;float importance=0;
  for(uint visited=0;visited<31&&count>0;++visited) {
    --count;
    LightNode node=nodes.data[pending[count]];
    uint level=levels[count];
    if(node.links.w<=1||level==4) {
      importance+=nodeWeight(nodes,pending[count],hit.p,hit.n)*max(frame.localScale.x,.25);
      if(importance>=.06)return .06;
    } else {
      pending[count]=node.links.y;levels[count]=level+1;
      pending[count+1]=node.links.x;levels[count+1]=level+1;
      count+=2;
    }
  }
  return importance;
}
// The configured sample and candidate counts are floors. Smoothly allocate
// one to four visibility samples using an independent random dimension; budget
// contours become fine sampling noise instead of coherent brightness steps.
// The budget is chosen before transport sampling and never uses its results.
uint adaptiveLocalBudget(LightNodes nodes,Hit hit,uint samples,uint state,out uint candidates) {
  candidates=max(uint(frame.localScale.z),1u);
  if((frame.localConfig.w&0x80000000u)==0||samples>=4)return samples;
  float importance=localImportance(nodes,hit);
  float target=1+smoothstep(.0025,.01,importance)+2*smoothstep(.015,.06,importance);
  uint budgetState=hash(state^0x4b1d5a77u);
  uint targetSamples=uint(floor(target))+uint(lightRandom(budgetState)<fract(target));
  return max(samples,targetSamples);
}
// Visibility needs the nearest transmitting sheet, but any accepted opaque
// intersection blocks the whole segment. Avoid closest-hit shading (including
// motion/previous vertices) and texture fetches for unconditional opaque hits.
struct LightOccluder { float t; vec3 p; vec3 n; vec3 transmission; };
bool localOccluder(accelerationStructureEXT scene,vec3 origin,vec3 direction,inout LightOccluder hit) {
  rayQueryEXT query;
  profileRay(16);
  // With no blended geometry, any hardware-opaque hit blocks the segment.
  // Scenes containing transmitting layers keep ordered closest-hit traversal.
  uint flags=(firstOpaqueHit?gl_RayFlagsTerminateOnFirstHitEXT:gl_RayFlagsNoneEXT)|traceFlags();
  rayQueryInitializeEXT(query,scene,flags,shadowMask(),origin,0,direction,hit.t);
  while(rayQueryProceedEXT(query)) {
    if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionTriangleEXT)continue;
    profileRay(17);
    Surface s=surfaces[rayQueryGetIntersectionInstanceCustomIndexEXT(query,false)+rayQueryGetIntersectionGeometryIndexEXT(query,false)];
    s.indices=s.traceIndices;
    if(enableDecals&&decalSurface(s))continue; // Paint does not cast a separate sheet shadow.
    if((s.state.w&4)==0&&!rayQueryGetIntersectionFrontFaceEXT(query,false)){profileRay(20);continue;}
    profileAlpha(s,16);
    if(!acceptCandidate(s,rayQueryGetIntersectionPrimitiveIndexEXT(query,false),rayQueryGetIntersectionBarycentricsEXT(query,false))){profileRay(19);continue;}
    if((s.state.w&2)==0) {rayQueryTerminateEXT(query);return true;}
    rayQueryConfirmIntersectionEXT(query);
  }
  if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT)return false;
  profileRay(21);
  hit.t=rayQueryGetIntersectionTEXT(query,true);
  Surface s=surfaces[rayQueryGetIntersectionInstanceCustomIndexEXT(query,true)+rayQueryGetIntersectionGeometryIndexEXT(query,true)];
  s.indices=s.traceIndices;
  if((s.state.w&2)==0)return true; // Hardware committed an opaque blocker.
  uint tri=rayQueryGetIntersectionPrimitiveIndexEXT(query,true)*3;
  vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,true);
  vec4 color=albedo(s,tri/3,bary);
  if(!blendedAtAlpha(s,color.a))return true;
  vec3 a=s.vertices.v[s.indices.i[tri]].position.xyz;
  vec3 b=s.vertices.v[s.indices.i[tri+1]].position.xyz;
  vec3 c=s.vertices.v[s.indices.i[tri+2]].position.xyz;
  mat4x3 model=rayQueryGetIntersectionObjectToWorldEXT(query,true);
  hit.p=model*vec4(a+bary.x*(b-a)+bary.y*(c-a),1);
  hit.n=normalize(cross(model*vec4(b-a,0),model*vec4(c-a,0)));
  if(dot(hit.n,direction)>0)hit.n=-hit.n;
  hit.transmission=sheetTransmission(s,color,hit.t);
  if((s.state.w&2048u)!=0)hit.transmission*=causticTint(s,tri/3,bary,hit.t);
  return false;
}
vec3 localVisibility(vec3 origin,vec3 endpoint) {
  vec3 transmission=vec3(1);
  for(uint layer=0;layer<32;++layer) {
    vec3 delta=endpoint-origin;float distance=length(delta);
    if(distance<=1e-6)return transmission;
    vec3 direction=delta/distance;
    if(shadowEarlyOut) {
      // Same layer result as localOccluder (visibility.glsl), stopping at the
      // first blocker; a sheet exactly at the endpoint is still ignored.
      VisibilityHit early=earlyVisibilityHit(origin,direction,distance,16u);
      if(early.blocked)return vec3(0);
      if(early.t==distance)return transmission;
      transmission*=early.transmission;
      origin=offsetRay(early.p,-early.n);
      continue;
    }
    LightOccluder hit;hit.t=distance;
    if((frame.target.z&1)!=0&&localOccluder(staticScene,origin,direction,hit))return vec3(0);
    if((frame.target.z&2)!=0&&localOccluder(dynamicScene,origin,direction,hit))return vec3(0);
    if(hit.t==distance)return transmission;
    transmission*=hit.transmission;
    origin=offsetRay(hit.p,-hit.n);
  }
  return vec3(0);
}
#include "light_guiding.glsl"
#include "regir.glsl"
// A sphere candidate includes solid-angle weighting, but not discrete selection.
struct LocalCandidate { vec3 physical; vec3 specular; vec3 endpoint; float distance; };
#include "triangle_sampling.glsl"
// IES profile (scene/ies.h): 256 samples from 0 to 180 degrees off the axis.
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer IesProfiles { float bins[]; };
float iesProfile(float profile,float cosine) {
  if(profile<.5||frame.iesProfiles==0)return 1;
  float x=acos(clamp(cosine,-1,1))*(255/3.14159265359);
  uint i=min(uint(x),254u),base=(uint(profile+.5)-1u)*256u+i;
  IesProfiles table=IesProfiles(frame.iesProfiles);
  return mix(table.bins[base],table.bins[base+1],x-float(i));
}
float emitterShaping(LightEmitter light,vec3 outgoing) {
  float cosine=dot(light.directionCone.xyz,outgoing);
  float outer=light.directionCone.w;
  float ies=iesProfile(light.radiance.w==0?light.corners[2].x:0,cosine); // spheres only
  if(outer<=-1)return ies*(light.shaping.y>0?pow(max(cosine,0),light.shaping.y):1);
  if(cosine<=outer)return 0;
  float inner=cos(acos(clamp(outer,-1,1))*(1-clamp(light.shaping.x,0,1)));
  float edge=inner>outer?smoothstep(outer,inner,cosine):1;
  return ies*edge*(light.shaping.y>0?pow(max(cosine,0),light.shaping.y):1);
}
// Light cookie of a sphere light: slot + 1 in corners[2].y, the projection's up
// and tan(width / 2) in corners[0]; the frame's table gives each slot's
// texture + 1. Dark outside the projection.
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer LightCookies { uint textures[]; };
vec3 emitterCookie(LightEmitter light,vec3 outgoing) {
  if(light.radiance.w!=0||light.corners[2].y<.5||frame.lightCookies==0)return vec3(1);
  vec3 axis=light.directionCone.xyz;
  float along=dot(axis,outgoing);
  if(along<=0)return vec3(0);
  vec3 up=light.corners[0].xyz,right=cross(axis,up);
  vec2 uv=vec2(dot(outgoing,right),dot(outgoing,up))/(along*light.corners[0].w)*.5+.5;
  if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1))))return vec3(0);
  uint index=LightCookies(frame.lightCookies).textures[uint(light.corners[2].y+.5)-1u];
  if(index==0u)return vec3(1);
  // Colour times alpha: a mask in either works (alpha-only cookies are common).
  vec4 texel=slotTexel(index-1u,textureLod(textures[nonuniformEXT(index-1u)],vec2(uv.x,1-uv.y),0));
  return texel.rgb*texel.a;
}
// What a light emits toward `outgoing`: shaping, IES profile and cookie.
vec3 emitterRadiance(LightEmitter light,vec3 outgoing) {
  return light.radiance.rgb*emitterShaping(light,outgoing)*emitterCookie(light,outgoing);
}
LocalCandidate triangleCandidate(LightEmitter light,Hit hit,inout uint state,bool angular,vec2 blue,uint stream) {
  LocalCandidate result;result.physical=result.specular=vec3(0);result.endpoint=hit.p;result.distance=0;
  vec3 a=light.corners[0].xyz,b=light.corners[1].xyz,c=light.corners[2].xyz;
  vec3 normal=cross(b-a,c-a);float twiceArea=length(normal);
  if(twiceArea<=1e-12)return result;
  normal/=twiceArea;
  vec2 sampleU=localEmitterSample(vec2(lightRandom(state),lightRandom(state)),blue,stream);
  vec3 endpoint;float omega=0;
  vec3 centerDelta=light.positionRadius.xyz-hit.p;
  bool spherical=false;
  if(enableAngularEmitters&&angular&&dot(centerDelta,centerDelta)<=16*light.positionRadius.w*light.positionRadius.w)
    spherical=sphericalTriangle(a,b,c,hit.p,sampleU,endpoint,omega);
  if(!spherical)endpoint=a+sqrt(sampleU.x)*((1-sampleU.y)*(b-a)+sampleU.y*(c-a));
  vec3 delta=endpoint-hit.p;float distance2=dot(delta,delta);
  if(distance2<=1e-12)return result;
  vec3 direction=delta*inversesqrt(distance2);
  float emitterCosine=dot(normal,-direction);
  if(light.radiance.w>1)emitterCosine=abs(emitterCosine);
  float geometry=max(0,dot(hit.n,direction))*max(0,emitterCosine);
  // Uniform area PDF 1/A, converted to solid angle: d^2/(A*cosEmitter).
  float integral=spherical?(emitterCosine>0?omega*max(0,dot(hit.n,direction))/3.14159265359:0)
                           :.5*twiceArea*geometry/(3.14159265359*distance2);
  vec3 scale=emitterRadiance(light,-direction)*integral
      *localFalloff(dot(light.positionRadius.xyz-hit.p,light.positionRadius.xyz-hit.p))*frame.localScale.x;
  result.physical=materialResponse(hit,direction,result.specular)*scale;
  result.specular*=scale;
  result.endpoint=offsetRay(endpoint,dot(normal,-direction)>0?normal:-normal);
  result.distance=sqrt(distance2);
  return result;
}
LocalCandidate localCandidate(LightEmitter light,Hit hit,inout uint state,bool angular,vec2 blue,uint stream) {
  if(light.radiance.w!=0)return triangleCandidate(light,hit,state,angular,blue,stream);
  LocalCandidate result;result.physical=result.specular=vec3(0);result.endpoint=hit.p;result.distance=0;
  vec3 delta=light.positionRadius.xyz-hit.p;
  float distance2=dot(delta,delta),radius=light.positionRadius.w;
  float attenuation=localFalloff(distance2);
  if(attenuation==0)return result;
  bool inside=distance2<=radius*radius;
  vec3 axis=distance2>1e-20?delta*inversesqrt(distance2):hit.n;
  float sinMax2=min(1,radius*radius/max(distance2,1e-20));
  // Stable for small, distant lights: 1-sqrt(1-x) = x/(1+sqrt(1-x)).
  float oneMinusCos=inside?2:sinMax2/(1+sqrt(max(0,1-sinMax2)));
  vec2 sampleU=localEmitterSample(vec2(lightRandom(state),lightRandom(state)),blue,stream);
  float cosTheta=1-sampleU.x*oneMinusCos;
  float sinTheta=sqrt(max(0,(1-cosTheta)*(1+cosTheta)));
  float phi=6.28318530718*sampleU.y;
  vec3 tangent=normalize(cross(abs(axis.z)<.999?vec3(0,0,1):vec3(0,1,0),axis));
  vec3 direction=normalize(axis*cosTheta+sinTheta*(cos(phi)*tangent+sin(phi)*cross(axis,tangent)));
  float cosine=max(0,dot(hit.n,direction));
  if(cosine==0)return result;
  float projected=dot(delta,direction);
  vec3 perpendicular=delta-direction*projected;
  float root=sqrt(max(0,radius*radius-dot(perpendicular,perpendicular)));
  float distance=max(0,inside?projected+root:projected-root);
  vec3 scale=emitterRadiance(light,-direction)*(2*oneMinusCos*cosine*attenuation)*frame.localScale.x;
  result.physical=materialResponse(hit,direction,result.specular)*scale;
  result.specular*=scale;
  result.endpoint=hit.p+direction*distance;result.distance=distance;
  return result;
}
float localLuminance(vec3 value) { return dot(value,vec3(.2126,.7152,.0722)); }
// Luminance-weighted direction (xyz) and weight (w) of the local-light samples
// at the opaque primary vertex: the SH direction of their contribution to the
// denoised PBR specular signal (addPbrLocalSpecular).
vec4 localLightDirection=vec4(0);
void localDirection(vec3 toward,vec3 contribution) {
#ifdef TEMPORAL
  float length2=dot(toward,toward),weight=localLuminance(contribution);
  if(!(length2>0)||!(weight>0)||!(weight<1e30))return;
  localLightDirection+=vec4(toward*(weight*inversesqrt(length2)),weight);
#endif
}
// The primary vertex's local light this frame with and without its shadow
// rays (luminance of the samples' estimates): their ratio is the share of the
// light that arrived. The next frame compares it (localOcclusionConfidence,
// pathtrace.comp). Recorded while the pass that compares it sets localProbing.
float localShadowed=0,localUnshadowed=0;bool localProbing=false;
void noteLocalProbe(vec3 visibility,vec3 estimate) {
#ifdef TEMPORAL
  float unshadowed=localLuminance(estimate);
  if(!localProbing||!(unshadowed>0)||!(unshadowed<1e30))return;
  localUnshadowed+=unshadowed;localShadowed+=localLuminance(estimate*visibility);
#endif
}
// Stratified light candidates for very small sets (RT Gems II, ch. 22-23).
// One sphere direction per light, then a weighted reservoir and ONE visibility
// query. E[physical / p] is the sum of these candidates; their expectation is
// the original sphere integral. Clamping, when requested, follows visibility and all weights.
vec3 smallLocalLighting(LightNodes nodes,LightEmitters emitters,Hit hit,uint state,uint samples,
                        uint guideKey,uint preferred,float guideMix,out float lightDistance,bool angular,vec2 blue,uint noiseBase,out vec3 specular,float threshold) {
  specular=vec3(0);vec3 result=vec3(0);float distanceWeight=0;lightDistance=0;
  for(uint sampleIndex=0;sampleIndex<samples;++sampleIndex) {
    LocalCandidate selected,preferredSample;
    selected.physical=selected.specular=vec3(0);selected.distance=0;selected.endpoint=hit.p;
    preferredSample=selected;
    uint selectedIndex=0;float total=0;
    uint nodeCount=2*nodes.data[0].links.w-1;
    for(uint index=0;index<nodeCount;++index) {
      if(nodes.data[index].links.w!=1)continue;
      if(!lightNodeVisible(nodes,index,hit.p,hit.n))continue;
      float share;uint emitter=leafEmitter(emitters,nodes.data[index].links,state,share);
      LocalCandidate candidate=localCandidate(emitters.data[emitter],hit,state,angular,blue,noiseBase+sampleIndex*8u+index);
      candidate.physical/=share;candidate.specular/=share;
      float weight=localLuminance(candidate.physical);total+=weight;
      if(weight>0&&lightRandom(state)*total<weight) {selected=candidate;selectedIndex=index;}
      if(index==preferred)preferredSample=candidate;
    }
    if(total<=0)continue;
    bool guided=guideMix>0&&localLuminance(preferredSample.physical)>0&&lightRandom(state)<guideMix;
    float mixWeight=localLuminance(preferredSample.physical)>0?guideMix:0;
    if(guided) {selected=preferredSample;selectedIndex=preferred;}
    float pdf=(1-mixWeight)*localLuminance(selected.physical)/total+(selectedIndex==preferred?mixWeight:0);
    vec3 estimate=selected.physical/pdf;
    vec3 visibility=localVisibility(offsetRay(hit.p,hit.ng),selected.endpoint);
    noteLocalProbe(visibility,estimate);
    vec3 contribution=estimate*visibility,specularContribution=selected.specular/pdf*visibility;
    clampLighting(hit,contribution,specularContribution,threshold);
    result+=contribution;specular+=specularContribution;
    if(angular)localDirection(selected.endpoint-hit.p,contribution);
    if(enableLocalGuiding&&guideKey!=0&&(hash(state^0xd1b54a35u)&63u)==0)
      trainLightGuide(guideKey,selectedIndex,guided,visibility,selected.physical,false);
    float weight=localLuminance(estimate);lightDistance+=selected.distance*weight;distanceWeight+=weight;
  }
  lightDistance=distanceWeight>0?lightDistance/distanceWeight:0;
  specular/=float(samples);return result/float(samples);
}
// Bounded stratified RIS for sets containing area emitters. Compare M fresh
// candidates using their actual unoccluded contribution, then trace only the
// reservoir winner. For e_i = physical_i / proposalPdf_i, weight_i = lum(e_i):
// E[winner * sum(weight)/(M*weight_winner) | candidates] = sum(e_i)/M.
// Stratifying the BVH variate preserves that expectation and reduces repeated
// draws from the same bright cluster. Visibility is always evaluated afresh;
// zero candidates remain in M. History changes only the proposal distribution;
// no radiance/shadow reuse or extra visibility rays.
vec3 resampledLocalLighting(LightNodes nodes,LightEmitters emitters,Hit hit,uint state,
                           uint samples,uint count,out float lightDistance,bool useGuide,vec2 blue,uint noiseBase,out vec3 specular,float threshold) {
  specular=vec3(0);vec3 result=vec3(0);float distanceWeight=0;lightDistance=0;
  uint guideKey=0,preferred=0,guideRoot=0;float guideMix=0,rootPdf=1;
  if(enableLocalGuiding&&useGuide&&frame.lightGuideWrite!=0) {
    guideKey=lightGuideKey(hit);
    guideMix=lightGuideProposal(nodes,hit,state,preferred,true);
    // Keep the stratified whole-tree proposal on torch-dominated cells. A
    // single cached torch is a poor substitute for the other visible torches.
    // (Sphere-only sets have no area lights to stratify: there, guidance is
    // the only visibility-aware proposal, e.g. lamps inside their fixtures.)
    if(guideMix>0&&emitters.data[nodes.data[preferred].links.z].radiance.w==0&&(frame.localConfig.w&0x40000000u)==0)guideMix=0;
    if(guideMix>0) {
      // A visible area light guides its local subtree, not just one triangle
      // of a tessellated surface.
      uint limit=32u;
      while(nodes.data[guideRoot].links.w>limit) {
        uvec4 links=nodes.data[guideRoot].links;
        float a=nodeWeight(nodes,links.x,hit.p,hit.n),b=nodeWeight(nodes,links.y,hit.p,hit.n);
        if(a==0&&b==0) {rootPdf=0;break;}
        float split=lightSplit(a,b);bool right=lightRank(nodes,preferred)>=lightRank(nodes,links.y);
        rootPdf*=right?1-split:split;guideRoot=right?links.y:links.x;
      }
      if(rootPdf<=0)guideMix=0;
    }
  }
  for(uint sampleIndex=0;sampleIndex<samples;++sampleIndex) {
    LocalCandidate selected;
    selected.physical=selected.specular=vec3(0);selected.endpoint=hit.p;selected.distance=0;
    float total=0;uint selectedIndex=0;bool selectedGuided=false;vec3 selectedPhysical=vec3(0);
    for(uint candidateIndex=0;candidateIndex<count;++candidateIndex) {
      bool guided=guideMix>0&&lightRandom(state)<guideMix;
      uint index=guided?guideRoot:0;float pdf=guided?rootPdf:1;
      float u=(float(candidateIndex)+lightRandom(state))/float(count);
      while(nodes.data[index].links.w>1) {
        uvec4 links=nodes.data[index].links;
        float a=nodeWeight(nodes,links.x,hit.p,hit.n),b=nodeWeight(nodes,links.y,hit.p,hit.n);
        if(a==0&&b==0) {pdf=0;break;}
        float probability=lightSplit(a,b);
        if(u<probability) {index=links.x;pdf*=probability;u/=probability;}
        else {index=links.y;pdf*=1-probability;u=(u-probability)/(1-probability);}
        u=min(u,.99999994);
      }
      bool inCluster=lightInSubtree(nodes,index,guideRoot);
      pdf*=1-guideMix+(guideMix>0&&inCluster?guideMix/rootPdf:0);
      if(pdf<=0)continue;
      float share;uint emitter=leafEmitter(emitters,nodes.data[index].links,state,share);
      pdf*=share;
      LocalCandidate candidate=localCandidate(emitters.data[emitter],hit,state,true,blue,noiseBase+sampleIndex*8u+candidateIndex);
      vec3 physical=candidate.physical;
      candidate.physical/=pdf;candidate.specular/=pdf;
      float weight=localLuminance(candidate.physical);total+=weight;
      if(weight>0&&lightRandom(state)*total<weight) {
        selected=candidate;selectedIndex=index;selectedGuided=guided;selectedPhysical=physical;
      }
    }
    if(total<=0)continue;
    vec3 estimate=selected.physical*(total/(float(count)*localLuminance(selected.physical)));
    vec3 visibility=localVisibility(offsetRay(hit.p,hit.ng),selected.endpoint);
    noteLocalProbe(visibility,estimate);
    vec3 contribution=estimate*visibility,specularContribution=selected.specular*(total/(float(count)*localLuminance(selected.physical)))*visibility;
    clampLighting(hit,contribution,specularContribution,threshold);
    result+=contribution;specular+=specularContribution;
    localDirection(selected.endpoint-hit.p,contribution);
    if(enableLocalGuiding&&guideKey!=0&&(hash(state^0xd1b54a35u)&63u)==0)
      trainLightGuide(guideKey,selectedIndex,selectedGuided,visibility,selectedPhysical,true);
    float weight=localLuminance(estimate);
    lightDistance+=selected.distance*weight;distanceWeight+=weight;
  }
  lightDistance=distanceWeight>0?lightDistance/distanceWeight:0;
  specular/=float(samples);return result/float(samples);
}
vec3 familyLocalLighting(LightNodes nodes,LightEmitters emitters,Hit hit,uint state,
                         out float distance,bool guide,uint noiseBase,out vec3 specular,float threshold) {
  visibilityCulling=guide;
  vec2 blue=localEmitterNoise(true);
  uint candidates=max(uint(frame.localScale.z),1u);
  if(nodes.data[0].links.w<=4)
    return smallLocalLighting(nodes,emitters,hit,state,1u,0u,0u,0,distance,true,blue,noiseBase,specular,threshold);
  return resampledLocalLighting(nodes,emitters,hit,state,1u,candidates,distance,guide,blue,noiseBase,specular,threshold);
}
// Separate direct signals. Equal-budget mode chooses a family with known
// probability and writes zero to the other signal. Per-family mode samples
// both. Each estimator retains its own PDF, hit distance and temporal history.
void splitLocalLighting(Hit hit,uint state,uint samples,out vec3 torch,out vec3 emission,
                        out float torchDistance,out float emissionDistance,out vec3 torchSpecular,out vec3 emissionSpecular) {
  torchSpecular=emissionSpecular=vec3(0);torch=emission=vec3(0);torchDistance=emissionDistance=0;
  if(!enableLocalLights||frame.localConfig.x==0||samples==0)return;
  bool hasTorch=false,hasEmission=false;
  if(frame.splitLocal.y>0)
    hasTorch=lightWeight(LightNodes(frame.torchNodes).data[0],hit.p,hit.n)>0;
  if(frame.splitLocal.z>0)
    hasEmission=lightWeight(LightNodes(frame.emissiveNodes).data[0],hit.p,hit.n)>0;
  // Choose the total budget once. Adapting inside each requested sample would
  // multiply a four-sample setting into as many as sixteen per family.
  uint adaptiveSamples=samples,candidates;
  if(hasTorch)
    adaptiveSamples=max(adaptiveSamples,adaptiveLocalBudget(LightNodes(frame.torchNodes),hit,samples,state,candidates));
  if(hasEmission)
    adaptiveSamples=max(adaptiveSamples,adaptiveLocalBudget(LightNodes(frame.emissiveNodes),hit,samples,state,candidates));
  float torchWeight=0,emissionWeight=0;
  state=hash(state^0x931a4bf5u);
  for(uint i=0;i<adaptiveSamples;++i) {
    vec3 oneTorch=vec3(0),oneEmission=vec3(0),oneTorchSpecular=vec3(0),oneEmissionSpecular=vec3(0);
    bool both=hasTorch&&hasEmission;
    // Complementary pixels provide each history with nearby valid samples.
    // A frame-wide random phase avoids a persistent spatial selection bias.
    uint phase=hash(frame.config.z+i*0x9e3779b9u)&1u;
    bool chooseTorch=frame.splitLocal.x==2||!both||((uint(shadingPixel.x)+uint(shadingPixel.y)+phase)&1u)==0u;
    float scale=frame.splitLocal.x==1&&both?2:1;
    if(hasTorch&&chooseTorch) {
      float distance;vec3 specular;
      vec3 value=familyLocalLighting(LightNodes(frame.torchNodes),LightEmitters(frame.torchEmitters),
          hit,hash(state^0x746f7263u),distance,false,128u+i*8u,specular,0)*scale;
      oneTorchSpecular=specular*scale;
      oneTorch=value;torchDistance+=distance;torchWeight+=distance>0?1:0;
    }
    if(hasEmission&&(frame.splitLocal.x==2||!both||!chooseTorch)) {
      float distance;vec3 specular;
      vec3 value=familyLocalLighting(LightNodes(frame.emissiveNodes),LightEmitters(frame.emissiveEmitters),
          hit,hash(state^0xe315ac47u),distance,true,256u+i*8u,specular,0)*scale;
      oneEmissionSpecular=specular*scale;
      oneEmission=value;emissionDistance+=distance;emissionWeight+=distance>0?1:0;
    }
    float limitScale=fireflyScale(hit,oneTorch+oneEmission,frame.firefly.x);
    torch+=oneTorch*limitScale;emission+=oneEmission*limitScale;
    torchSpecular+=oneTorchSpecular*limitScale;emissionSpecular+=oneEmissionSpecular*limitScale;
    state=hash(state+0x9e3779b9u);
  }
  torch/=float(adaptiveSamples);emission/=float(adaptiveSamples);
  torchSpecular/=float(adaptiveSamples);emissionSpecular/=float(adaptiveSamples);
  // Occlusion is a valid black sample, not a missing sample. Keep its distance
  // so the filter does not mistake blocked rays for skipped family selections.
  torchDistance=torchWeight>0?torchDistance/torchWeight:0;
  emissionDistance=emissionWeight>0?emissionDistance/emissionWeight:0;
}
void splitLocalLighting(Hit hit,uint state,uint samples,out vec3 torch,out vec3 emission,
                        out float torchDistance,out float emissionDistance) {
  vec3 torchSpecular,emissionSpecular;
  splitLocalLighting(hit,state,samples,torch,emission,torchDistance,emissionDistance,torchSpecular,emissionSpecular);
}
// One level of a sample's descent from its children's weights (a, b), all
// selects; a finished walk (walking false) keeps its state.
void lightStep(float a,float b,uvec2 children,inout uint index,inout float pdf,inout float u,inout bool walking) {
  bool dead=a==0&&b==0,step=walking&&!dead;
  float probability=lightSplit(a,b);
  bool left=u<probability;
  float share=left?probability:1-probability;
  index=step?(left?children.x:children.y):index;
  pdf=walking?(dead?0:pdf*share):pdf;
  u=step?min((left?u:u-probability)/share,0.99999994):u;
  walking=step;
}
// Two samples' descents, interleaved. A level is a chain of dependent loads
// and weight math that a single walk cannot overlap (the pass stalled on
// instruction dependencies, not memory); the second chain fills that latency.
// Each walk is exactly the one-sample walk. ub < 0: walk only the first.
void lightWalkPair(LightNodes nodes,vec3 p,vec3 n,float ua,float ub,
                   out uint indexA,out float pdfA,out uint indexB,out float pdfB) {
  indexA=indexB=0;pdfA=1;pdfB=ub<0?-1:1;
  bool activeA=true,activeB=ub>=0,table=visibilityTableApplies(nodes,p,n);
  uvec2 rootChildren=nodes.data[0].links.xy;
  for(;;) {
    uvec4 linksA=nodes.data[indexA].links,linksB=nodes.data[indexB].links;
    activeA=activeA&&linksA.w>1;activeB=activeB&&linksB.w>1;
    if(!activeA&&!activeB)break;
    // A finished walk weighs the root's children: valid, discarded loads.
    uvec2 childrenA=activeA?linksA.xy:rootChildren,childrenB=activeB?linksB.xy:rootChildren;
    float a0=lightNodeVisibility(nodes,childrenA.x,table)*lightWeight(nodes.data[childrenA.x],p,n);
    float a1=lightNodeVisibility(nodes,childrenA.y,table)*lightWeight(nodes.data[childrenA.y],p,n);
    float b0=lightNodeVisibility(nodes,childrenB.x,table)*lightWeight(nodes.data[childrenB.x],p,n);
    float b1=lightNodeVisibility(nodes,childrenB.y,table)*lightWeight(nodes.data[childrenB.y],p,n);
    lightStep(a0,a1,childrenA,indexA,pdfA,ua,activeA);
    lightStep(b0,b1,childrenB,indexB,pdfB,ub,activeB);
  }
}
bool regirLookup=true; // localLighting may use ReGIR pools (false at directly seen surfaces)
// Returns material-demodulated total radiance and its specular component.
vec3 localLighting(Hit hit,uint state,uint samples,out float lightDistance,bool useGuiding,out vec3 specular,float threshold) {
  specular=vec3(0);lightDistance=0;
  visibilityCulling=useGuiding;
  if(!enableLocalLights||frame.localConfig.x==0||samples==0)return vec3(0);
  LightNodes nodes=LightNodes(frame.lightNodes);
  LightEmitters emitters=LightEmitters(frame.lightEmitters);
  if(nodeWeight(nodes,0,hit.p,hit.n)<=0)return vec3(0);
  vec2 blue=localEmitterNoise(useGuiding);
  uint adaptiveSamples=samples,adaptiveCandidates=uint(frame.localScale.z);
  if(useGuiding)
    adaptiveSamples=adaptiveLocalBudget(nodes,hit,samples,state,adaptiveCandidates);
  // The opaque primary vertex has its own signal/filter. Keep the bounded
  // continuation and transparent-sheet stack on the established estimator.
  if(enableLightResampling&&useGuiding&&frame.localScale.z>1)
    return resampledLocalLighting(nodes,emitters,hit,hash(state^0x746f7263u),adaptiveSamples,
                                  adaptiveCandidates,lightDistance,true,blue,0u,specular,threshold);
  uint guideKey=0,preferred=0;
  float guideMix=0,preferredPdf=-1;
  if(enableLocalGuiding&&useGuiding&&frame.lightGuideWrite!=0) {
    guideKey=lightGuideKey(hit);
    guideMix=lightGuideProposal(nodes,hit,state,preferred,false);
  }
  state=hash(state^0x746f7263u);
  if(enableSmallLights&&nodes.data[0].links.w<=4)
    return smallLocalLighting(nodes,emitters,hit,state,adaptiveSamples,guideKey,preferred,guideMix,lightDistance,useGuiding,blue,0u,specular,threshold);
  vec3 result=vec3(0);float distanceWeight=0;
  // Bounce vertices draw from the cell's presampled pool (regir.glsl). Not
  // directly seen surfaces (regirLookup false: transparent sheets): every
  // pixel looking up a cell shares its few slots for the frame, so a slot
  // whose light the cell rates unlikely but is near lit about one pixel in
  // K with a large weight, cell-sized splotches the denoiser kept.
  uint pool=useGuiding||!regirLookup?regirNone:regirPool(hit.p,state);
  // The tree (or pool) variates of the samples that are not guided are
  // stratified, (j + xi)/walked: samples no longer pile onto one bright
  // cluster by chance. Each keeps its marginal distribution, so every
  // estimate and PDF is as before; only their sum varies less.
  uint guideSeed=hash(state^0x67756964u),walkSeed=hash(state^0x7374726au),walked=0,stratum=0;
  // Tree walks go in pairs: the next unguided sample's (stratum's) leaf and
  // PDF wait here (pendingPdf < 0: none).
  uint pendingIndex=0;float pendingPdf=-1;
  for(uint sampleIndex=0;sampleIndex<adaptiveSamples;++sampleIndex) {
    uint guideState=hash(guideSeed+sampleIndex);
    walked+=guideMix>0&&lightRandom(guideState)<guideMix?0u:1u;
  }
  for(uint sampleIndex=0;sampleIndex<adaptiveSamples;++sampleIndex) {
    uint guideState=hash(guideSeed+sampleIndex);
    bool guided=guideMix>0&&lightRandom(guideState)<guideMix;
    uint index=0;float selectionPdf=1,u=0;
    if(!guided) {
      uint strataState=hash(walkSeed+stratum*0x9e3779b9u);
      u=min((float(stratum++)+lightRandom(strataState))/float(walked),.99999994);
    }
    if(guided) {
      if(preferredPdf<0)preferredPdf=lightBasePdf(nodes,hit,preferred);
      index=preferred;selectionPdf=preferredPdf;
    }
    else if(pool!=regirNone) {
      if(!regirSample(pool,u,index,selectionPdf))continue;
    }
    else if(pendingPdf>=0) {index=pendingIndex;selectionPdf=pendingPdf;pendingPdf=-1;}
    else {
      // The next stratum's variate, exactly as its own sample draws it.
      float next=-1;
      if(stratum<walked) {
        uint strataState=hash(walkSeed+stratum*0x9e3779b9u);
        next=min((float(stratum)+lightRandom(strataState))/float(walked),.99999994);
      }
      lightWalkPair(nodes,hit.p,hit.n,u,next,index,selectionPdf,pendingIndex,pendingPdf);
    }
    // Full mixture PDF, including the BVH component even for a guided draw.
    selectionPdf=(1-guideMix)*selectionPdf+(index==preferred?guideMix:0);
    if(selectionPdf<=0)continue;
    float share;uint emitter=leafEmitter(emitters,nodes.data[index].links,state,share);
    selectionPdf*=share;
    LightEmitter light=emitters.data[emitter];
    // The sample's light (physical, its specular share), point and distance;
    // a triangle or a sphere, then one visibility call site for both.
    vec3 physical,physicalSpecular,endpoint;float distance;
    if(light.radiance.w!=0) {
      LocalCandidate candidate=triangleCandidate(light,hit,state,useGuiding,blue,sampleIndex*8u);
      physical=candidate.physical;physicalSpecular=candidate.specular;endpoint=candidate.endpoint;distance=candidate.distance;
    } else {
      vec3 delta=light.positionRadius.xyz-hit.p;
      float distance2=dot(delta,delta),radius=light.positionRadius.w;
      float attenuation=localFalloff(distance2);
      if(attenuation==0)continue;
      bool inside=distance2<=radius*radius;
      vec3 axis=distance2>1e-20?delta*inversesqrt(distance2):hit.n;
      float sinMax2=min(1,radius*radius/max(distance2,1e-20));
      // Stable for small, distant lights: 1-sqrt(1-x) = x/(1+sqrt(1-x)).
      float oneMinusCos=inside?2:sinMax2/(1+sqrt(max(0,1-sinMax2)));
      vec2 sampleU=localEmitterSample(vec2(lightRandom(state),lightRandom(state)),blue,sampleIndex*8u);
      float cosTheta=1-sampleU.x*oneMinusCos;
      float sinTheta=sqrt(max(0,(1-cosTheta)*(1+cosTheta)));
      float phi=6.28318530718*sampleU.y;
      vec3 tangent=normalize(cross(abs(axis.z)<.999?vec3(0,0,1):vec3(0,1,0),axis));
      vec3 direction=normalize(axis*cosTheta+sinTheta*(cos(phi)*tangent+sin(phi)*cross(axis,tangent)));
      float cosine=max(0,dot(hit.n,direction));
      if(cosine==0)continue;
      float projected=dot(delta,direction);
      vec3 perpendicular=delta-direction*projected;
      float root=sqrt(max(0,radius*radius-dot(perpendicular,perpendicular)));
      distance=max(0,inside?projected+root:projected-root);
      vec3 scale=emitterRadiance(light,-direction)*(2*oneMinusCos*cosine*attenuation)*frame.localScale.x;
      physical=materialResponse(hit,direction,physicalSpecular)*scale;physicalSpecular*=scale;
      endpoint=hit.p+direction*distance;
    }
    // A spot light facing away or a back-facing response: no visibility ray.
    if(localLuminance(physical)<=0)continue;
    vec3 estimate=physical/selectionPdf;
    vec3 visibility=localVisibility(offsetRay(hit.p,hit.ng),endpoint);
    noteLocalProbe(visibility,estimate);
    vec3 contribution=estimate*visibility,specularContribution=physicalSpecular/selectionPdf*visibility;
    clampLighting(hit,contribution,specularContribution,threshold);
    result+=contribution;specular+=specularContribution;
    if(useGuiding)localDirection(endpoint-hit.p,contribution);
    if(enableLocalGuiding&&useGuiding&&guideKey!=0&&(hash(state^0xd1b54a35u)&63u)==0)
      trainLightGuide(guideKey,index,guided,visibility,physical,false);
    float weight=localLuminance(estimate);
    lightDistance+=distance*weight;distanceWeight+=weight;
  }
  lightDistance=distanceWeight>0?lightDistance/distanceWeight:0;
  specular/=float(adaptiveSamples);return result/float(adaptiveSamples);
}
vec3 localLighting(Hit hit,uint state,uint samples,out float lightDistance,bool useGuiding,out vec3 specular) {
  return localLighting(hit,state,samples,lightDistance,useGuiding,specular,useGuiding?frame.firefly.x:0);
}
vec3 localLighting(Hit hit,uint state,uint samples,out float lightDistance,bool useGuiding) {
  vec3 specular;
  return localLighting(hit,state,samples,lightDistance,useGuiding,specular);
}
vec3 localLighting(Hit hit,uint state,uint samples,out float lightDistance) {
  return localLighting(hit,state,samples,lightDistance,false);
}
