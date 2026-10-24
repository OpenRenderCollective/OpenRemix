// ReSTIR DI for primary local lighting (Bitterli et al. 2020; the RTXDI
// structure): each pixel keeps a reservoir holding one light sample and its
// unbiased contribution weight W. A frame resamples the light tree
// (splitRestirInitial: RIS over candidates, a visibility test of the winner,
// merge with the reprojected previous reservoir), then neighbors
// (splitRestirSpatial), and shades the winner with one shadow ray. Occluded
// winners are dropped from the history so they are not reused.
//
// Samples are points on lights in area measure, stored as the emitter index
// and two coordinates (sphere: octahedral surface direction; triangle:
// barycentrics), so reuse at another pixel or frame evaluates the same point.
// Spatial reuse normalizes by the M of the reservoirs that could have produced
// the chosen sample (RTXDI's basic 1/Z bias correction, geometric test, no
// rays); temporal reuse merges without correction.
// pad: in the current buffer, this frame's occlusion change of the history
// sample (restirOcclusionChange); in history, 1 marks a sample that was
// occluded when shaded (kept only to detect it becoming visible).
struct Reservoir { uint light; float u, v, W, M, depth; uint normal; float target; float x, y, z, pad; };
layout(buffer_reference,std430,buffer_reference_align=16) buffer Reservoirs { Reservoir data[]; };
layout(buffer_reference,std430,buffer_reference_align=4) readonly buffer LightRemap { uint data[]; };
const uint restirNone=0xffffffffu;

Reservoir restirEmpty() {
  Reservoir r;r.light=restirNone;r.u=r.v=r.W=r.M=r.depth=r.target=r.x=r.y=r.z=r.pad=0;r.normal=0;return r;
}
uint restirPackNormal(vec3 n) { return packSnorm2x16(encodeGeometricNormal(n)); }
vec3 restirUnpackNormal(uint n) { return decodeGeometricNormal(unpackSnorm2x16(n)); }

struct RestirPoint { vec3 x; vec3 n; bool twoSided; };
RestirPoint restirPoint(LightEmitter light,vec2 uv) {
  RestirPoint p;
  if(light.radiance.w==0) {
    p.n=decodeGeometricNormal(uv);
    p.x=light.positionRadius.xyz+p.n*light.positionRadius.w;
    p.twoSided=false;
  } else {
    vec3 a=light.corners[0].xyz,b=light.corners[1].xyz,c=light.corners[2].xyz;
    p.x=a+uv.x*(b-a)+uv.y*(c-a);
    p.n=normalize(cross(b-a,c-a));
    p.twoSided=light.radiance.w>1;
  }
  return p;
}

// Unshadowed contribution of a light point at a surface, area measure:
// response * Le * cos_s * cos_x / (pi d^2), as the tree estimators integrate.
vec3 restirContribution(Hit hit,LightEmitter light,vec2 uv,out vec3 specular,out vec3 endpoint,out float distance) {
  specular=vec3(0);endpoint=hit.p;distance=0;
  RestirPoint p=restirPoint(light,uv);
  vec3 delta=p.x-hit.p;float d2=dot(delta,delta);
  if(d2<=1e-12)return vec3(0);
  distance=sqrt(d2);vec3 w=delta/distance;
  float cosS=dot(hit.n,w);
  float cosX=dot(p.n,-w);if(p.twoSided)cosX=abs(cosX);
  if(cosS<=0||cosX<=0)return vec3(0);
  float attenuation=localFalloff(dot(light.positionRadius.xyz-hit.p,light.positionRadius.xyz-hit.p));
  vec3 scale=light.radiance.rgb*emitterShaping(light,-w)*(cosS*cosX*attenuation/(3.14159265359*d2))*frame.localScale.x;
  vec3 physical=materialResponse(hit,w,specular)*scale;
  specular*=scale;
  // Sphere lights are not geometry; triangle emitters are, so stop short of them.
  endpoint=light.radiance.w==0?p.x:offsetRay(p.x,dot(p.n,-w)>0?p.n:-p.n);
  return physical;
}
// Whether a surface (position, normal) could have a nonzero target for the
// light point: it faces the point, the point's side emits toward it, and it
// is inside the light's cone and reach. (1/Z's support test.)
bool restirReaches(vec3 p,vec3 n,LightEmitter light,vec2 uv) {
  RestirPoint q=restirPoint(light,uv);
  vec3 d=q.x-p;float d2=dot(d,d);
  if(d2<=1e-12)return false;
  vec3 w=d*inversesqrt(d2);
  float cosX=dot(q.n,-w);if(q.twoSided)cosX=abs(cosX);
  return dot(n,w)>0&&cosX>0&&emitterShaping(light,-w)>0&&
         localFalloff(dot(light.positionRadius.xyz-p,light.positionRadius.xyz-p))>0;
}
float restirTarget(Hit hit,LightEmitter light,vec2 uv) {
  vec3 specular,endpoint;float distance;
  return localLuminance(restirContribution(hit,light,uv,specular,endpoint,distance));
}

// A point on the light and its area-measure PDF: the visible cone of a
// sphere (solid-angle uniform), or uniform area on a triangle.
bool restirSamplePoint(LightEmitter light,Hit hit,vec2 u,out vec2 uv,out float pdf) {
  uv=vec2(0);pdf=0;
  if(light.radiance.w==0) {
    vec3 center=light.positionRadius.xyz;float radius=light.positionRadius.w;
    vec3 delta=center-hit.p;float d2=dot(delta,delta);
    if(radius<=0||d2<=radius*radius)return false;
    vec3 axis=delta*inversesqrt(d2);
    float sinMax2=min(1,radius*radius/d2);
    float oneMinusCos=sinMax2/(1+sqrt(max(0,1-sinMax2)));
    float cosTheta=1-u.x*oneMinusCos;
    float sinTheta=sqrt(max(0,(1-cosTheta)*(1+cosTheta)));
    float phi=6.28318530718*u.y;
    vec3 tangent=normalize(cross(abs(axis.z)<.999?vec3(0,0,1):vec3(0,1,0),axis));
    vec3 w=normalize(axis*cosTheta+sinTheta*(cos(phi)*tangent+sin(phi)*cross(axis,tangent)));
    float projected=dot(delta,w);
    vec3 perpendicular=delta-w*projected;
    float t=projected-sqrt(max(0,radius*radius-dot(perpendicular,perpendicular)));
    if(t<=0)return false;
    vec3 n=normalize(hit.p+w*t-center);
    float cosX=abs(dot(n,-w));
    if(cosX<=1e-6)return false;
    uv=encodeGeometricNormal(n);
    pdf=cosX/(6.28318530718*oneMinusCos*t*t);
  } else {
    vec3 a=light.corners[0].xyz,b=light.corners[1].xyz,c=light.corners[2].xyz;
    float area=.5*length(cross(b-a,c-a));
    if(area<=1e-12)return false;
    float s=sqrt(u.x);
    uv=vec2(s*(1-u.y),s*u.y);
    pdf=1/area;
  }
  return true;
}

// Resampling step: stream reservoir r (its target at this pixel is target)
// into the running sum. A reservoir without a sample (nothing found, or its
// sample was occluded) still counts its M: it is a zero-valued estimate, and
// dropping it would average only the successful ones (brightening).
void restirStream(inout Reservoir result,inout float wsum,inout float selected,Reservoir r,float target,inout uint state) {
  float w=r.light!=restirNone?target*r.W*r.M:0;
  result.M+=r.M;
  if(w<=0)return;
  wsum+=w;
  if(lightRandom(state)*wsum<w){result.light=r.light;result.u=r.u;result.v=r.v;selected=target;}
}
void restirFinish(inout Reservoir r,float wsum,float selected) {
  r.W=selected>0&&r.M>0?wsum/(r.M*selected):0;
  r.target=selected;
  if(r.W<=0)r.light=restirNone;
}

// Initial candidates and temporal reuse (splitRestirInitial).
Reservoir restirInitial(Hit hit,uint state,float depth,ivec2 pixel) {
  LightNodes nodes=LightNodes(frame.lightNodes);
  LightEmitters emitters=LightEmitters(frame.lightEmitters);
  Reservoir r=restirEmpty();
  if(frame.localConfig.x==0||nodeWeight(nodes,0,hit.p,hit.n)<=0)return r;
  uint count=max(frame.restirConfig.x,1u);
  float wsum=0,selected=0;
  // Each candidate descends the light tree with a stratified variate, using
  // the local-light estimators' weights (nodeWeight). The variates increase
  // with i, so consecutive descents share an upper path and leave it once
  // (leaf order is monotonic in u). The previous path's split probabilities
  // are kept per level and reused while the paths coincide: identical PMFs,
  // a fraction of the node weights (RT Gems I, 18.4.3.2).
  const uint pathCapacity=24u;
  uint pathNode[pathCapacity];float pathSplit[pathCapacity];uint pathLength=0;
  uint pool=regirPool(hit.p,state);
  for(uint i=0;i<count;++i) {
    float pmf=1,u=(float(i)+lightRandom(state))/float(count);
    uint index=0,level=0;bool onPath=true;
    // A presampled cell pool replaces the descent (regir.glsl).
    if(pool!=regirNone&&!regirSample(pool,u,index,pmf))pmf=0;
    else while(pool==regirNone&&nodes.data[index].links.w>1) {
      uvec4 links=nodes.data[index].links;
      float p;
      if(onPath&&level<pathLength&&pathNode[level]==index)p=pathSplit[level];
      else {
        onPath=false;
        float a=nodeWeight(nodes,links.x,hit.p,hit.n),b=nodeWeight(nodes,links.y,hit.p,hit.n);
        if(a==0&&b==0){pmf=0;pathLength=min(level,pathCapacity);break;}
        p=lightSplit(a,b);
        if(level<pathCapacity){pathNode[level]=index;pathSplit[level]=p;pathLength=level+1;}
      }
      if(u<p){index=links.x;pmf*=p;u/=p;}else{index=links.y;pmf*=1-p;u=(u-p)/(1-p);}
      u=min(u,.99999994);++level;
    }
    if(pmf<=0)continue;
    float share;uint emitter=leafEmitter(emitters,nodes.data[index].links,state,share);
    pmf*=share;
    LightEmitter light=emitters.data[emitter];
    vec2 uv;float pdf;
    if(!restirSamplePoint(light,hit,vec2(lightRandom(state),lightRandom(state)),uv,pdf))continue;
    float target=restirTarget(hit,light,uv);
    float w=target/(pmf*pdf);
    if(w<=0)continue;
    wsum+=w;
    if(lightRandom(state)*wsum<w){r.light=emitter;r.u=uv.x;r.v=uv.y;selected=target;}
  }
  // One initial reservoir counts as M=1 (RTXDI): temporal history is then a
  // number of frames.
  r.M=1;
  r.W=selected>0?wsum/(float(count)*selected):0;
  if(r.W<=0)r.light=restirNone;
  if(r.light!=restirNone&&(frame.restirConfig.w&1u)!=0) {
    vec3 specular,endpoint;float distance;
    restirContribution(hit,emitters.data[r.light],vec2(r.u,r.v),specular,endpoint,distance);
    if(localLuminance(localVisibility(offsetRay(hit.p,hit.ng),endpoint))<=0){r.W=0;r.light=restirNone;}
  }
  r.target=selected;
  float occlusionChange=0;
  // Temporal reuse: the reservoir the same surface held last frame.
  // The opaque pass's motion vector (guideMotion: previous-frame offset in
  // pixels, previous view depth minus current, valid in w) locates last
  // frame's surface. Raster primary visibility cannot use hit.previousPosition
  // here: the opaque pass reuses that image for emission.
  vec4 motion=imageLoad(guideMotion,pixel);
  if(frame.restirHistoryRead!=0&&motion.w!=0) {
    {
      ivec2 at=ivec2(floor(vec2(pixel)+.5+frame.jitter.xy+motion.xy-temporal.jitter.zw));
      // Permutation sampling: a bijective per-frame shuffle within 2x2 blocks.
      if((frame.restirConfig.w&2u)!=0)at^=ivec2(frame.config.z&1u,(frame.config.z>>1)&1u);
      if(all(greaterThanEqual(at,ivec2(0)))&&all(lessThan(at,ivec2(frame.target.xy)))) {
        Reservoir p=Reservoirs(frame.restirHistoryRead).data[uint(at.x)+uint(at.y)*frame.target.x];
        float previousDepth=depth+motion.z;
        bool similar=p.M>0&&abs(p.depth-previousDepth)<=.1*previousDepth&&
                     dot(restirUnpackNormal(p.normal),hit.n)>.8;
        if(similar&&p.light!=restirNone&&frame.restirRemap!=0)p.light=LightRemap(frame.restirRemap).data[p.light];
        if(p.light!=restirNone&&p.light>=uint(frame.restirParams.y))p.light=restirNone; // light gone
        // Re-test the history sample's segment from the same surface point.
        // Visible then and blocked now, or blocked then (flagged) and
        // visible now: an occluder moved (a door), and the local denoiser's
        // history there is stale. A static scene cannot change this test,
        // so penumbrae and permutation sampling never register.
        bool wasOccluded=p.pad>.5;
        if(similar&&p.light!=restirNone&&(frame.restirConfig.w&4u)!=0) {
          LightEmitter light=emitters.data[p.light];
          RestirPoint q=restirPoint(light,vec2(p.u,p.v));
          vec3 point=vec3(p.x,p.y,p.z),normal=restirUnpackNormal(p.normal);
          vec3 endpoint=light.radiance.w==0?q.x:offsetRay(q.x,dot(q.n,point-q.x)>0?q.n:-q.n);
          float visible=clamp(localLuminance(localVisibility(offsetRay(point,normal),endpoint)),0,1);
          occlusionChange=wasOccluded?visible:1-visible;
        }
        if(wasOccluded)p.light=restirNone; // never reuse an occluded sample
        if(similar) {
          p.M=min(p.M,float(frame.restirConfig.z)*max(r.M,1));
          Reservoir merged=restirEmpty();float mergedSum=0,mergedTarget=0;
          restirStream(merged,mergedSum,mergedTarget,r,r.target,state);
          restirStream(merged,mergedSum,mergedTarget,p,p.light!=restirNone?restirTarget(hit,emitters.data[p.light],vec2(p.u,p.v)):0,state);
          restirFinish(merged,mergedSum,mergedTarget);
          r=merged;
        }
      }
    }
  }
  r.pad=occlusionChange;
  r.depth=depth;r.normal=restirPackNormal(hit.n);r.x=hit.p.x;r.y=hit.p.y;r.z=hit.p.z;
  return r;
}

float restirOcclusionChange=0; // set by restirShade
// Spatial reuse and shading (splitRestirSpatial). Returns the pixel's local
// radiance; writes the reservoir kept for the next frame.
vec3 restirShade(Hit hit,ivec2 pixel,uint state,float depth,out float lightDistance,out vec3 specular) {
  specular=vec3(0);lightDistance=0;
  LightEmitters emitters=LightEmitters(frame.lightEmitters);
  Reservoirs current=Reservoirs(frame.restirCurrent);
  uint self=uint(pixel.x)+uint(pixel.y)*frame.target.x;
  Reservoir own=current.data[self];
  // Occlusion change around the pixel (3x3 maximum): sparse per-pixel
  // detections cover the region whose light changed.
  restirOcclusionChange=0;
  if((frame.restirConfig.w&4u)!=0)
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
      ivec2 at=clamp(pixel+ivec2(x,y),ivec2(0),ivec2(frame.target.xy)-1);
      restirOcclusionChange=max(restirOcclusionChange,current.data[uint(at.x)+uint(at.y)*frame.target.x].pad);
    }
  Reservoir r=restirEmpty();float wsum=0,selected=0;
  restirStream(r,wsum,selected,own,own.target,state);
  const uint maxNeighbors=8u;
  uint neighbors[maxNeighbors];uint used=0;
  for(uint i=0;i<min(frame.restirConfig.y,maxNeighbors);++i) {
    float angle=6.28318530718*lightRandom(state),radius=frame.restirParams.x*sqrt(lightRandom(state));
    ivec2 at=pixel+ivec2(round(vec2(cos(angle),sin(angle))*radius));
    if(at==pixel||any(lessThan(at,ivec2(0)))||any(greaterThanEqual(at,ivec2(frame.target.xy))))continue;
    uint index=uint(at.x)+uint(at.y)*frame.target.x;
    Reservoir n=current.data[index];
    if(n.M<=0||abs(n.depth-depth)>.1*depth||dot(restirUnpackNormal(n.normal),hit.n)<.8)continue;
    neighbors[used++]=index;
    restirStream(r,wsum,selected,n,n.light!=restirNone?restirTarget(hit,emitters.data[n.light],vec2(n.u,n.v)):0,state);
  }
  // 1/Z: count only reservoirs whose surface could have produced the sample.
  float z=r.M;
  if(r.light!=restirNone&&selected>0) {
    z=own.M;
    LightEmitter light=emitters.data[r.light];
    for(uint i=0;i<used;++i) {
      Reservoir n=current.data[neighbors[i]];
      if(restirReaches(vec3(n.x,n.y,n.z),restirUnpackNormal(n.normal),light,vec2(r.u,r.v)))z+=n.M;
    }
  }
  float totalM=r.M;
  r.M=z;
  restirFinish(r,wsum,selected);
  r.M=totalM;
  vec3 radiance=vec3(0);
  if(r.light!=restirNone) {
    vec3 endpoint;
    vec3 physical=restirContribution(hit,emitters.data[r.light],vec2(r.u,r.v),specular,endpoint,lightDistance);
    vec3 visibility=localVisibility(offsetRay(hit.p,hit.ng),endpoint);
    radiance=physical*r.W*visibility;specular*=r.W*visibility;
    localDirection(endpoint-hit.p,radiance);
    // Occluded samples are never reused (RTXDI discardInvisibleSamples); the
    // history keeps them flagged so a shadow that clears can be detected.
    r.pad=localLuminance(visibility)<=0?1:0;
  }
  r.depth=depth;r.normal=restirPackNormal(hit.n);r.x=hit.p.x;r.y=hit.p.y;r.z=hit.p.z;
  Reservoirs(frame.restirHistoryWrite).data[self]=r;
  return radiance;
}
