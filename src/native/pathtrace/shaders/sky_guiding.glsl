// Sky guiding (sky_guiding.cpp). A world cell and facing (the light guide's
// cell key, its own grid and table) keeps a histogram of the directions its
// first-surface sky rays found the sky in: 64 equal solid-angle bins of the
// hemisphere around the facing's axis (concentric map, Shirley and Chiu),
// each adding the ray's visible sky luminance over its sampling PDF, so a
// bin holds an estimate of the sky light it delivers. The extra sky rays
// draw from it with the configured share, the rest by the BRDF, and are
// weighted by the mixture PDF: the guide changes no expectation, and the
// BRDF share reaches the directions it has not seen.
// A slot: key, total, two reserved words, 64 bins (fixed point, 1/16 unit).
layout(buffer_reference,std430,buffer_reference_align=4) buffer SkyGuide { uint data[]; };
const uint skyGuideStride=68u,skyGuideBins=64u;
const float skyGuideUnit=16.;
// A share of the proposal spread over every bin: a bin that has not found the
// sky yet keeps being probed by guided rays too.
const float skyGuideUniform=.1;

bool skyGuideActive() { return frame.skyGuideConfig.w!=0u; }
float skyGuideScale() { return uintBitsToFloat(frame.skyGuideConfig.y); }
float skyGuideShare() { return uintBitsToFloat(frame.skyGuideConfig.z); }

// The facing's frame: z along its axis (lightGuideFace: axis*2 + positive).
mat3 skyGuideFrame(uint face) {
  vec3 z=vec3(0);z[face/2u]=(face&1u)!=0u?1.:-1.;
  vec3 x=vec3(0);x[(face/2u+1u)%3u]=1.;
  return mat3(x,cross(z,x),z);
}
vec2 skyGuideDisk(vec2 u) {
  vec2 a=2*u-1;
  if(a.x==0&&a.y==0)return vec2(0);
  float r,phi;
  if(abs(a.x)>abs(a.y)){r=a.x;phi=.785398163*(a.y/a.x);}
  else{r=a.y;phi=1.570796327-.785398163*(a.x/a.y);}
  return r*vec2(cos(phi),sin(phi));
}
vec2 skyGuideSquare(vec2 d) {
  float r=length(d);
  if(r==0)return vec2(.5);
  float theta=atan(d.y,d.x);
  if(theta<-.785398163)theta+=6.283185307;
  vec2 a;
  if(theta<.785398163)a=vec2(r,r*theta/.785398163);
  else if(theta<2.356194490)a=vec2(r*(1.570796327-theta)/.785398163,r);
  else if(theta<3.926990817)a=vec2(-r,-r*(theta-3.141592654)/.785398163);
  else a=vec2(-r*(4.712388980-theta)/.785398163,-r);
  return clamp(a*.5+.5,0,1);
}
// Uniform over the hemisphere (PDF 1/2pi): z = 1 - r^2.
vec3 skyGuideHemisphere(vec2 u) {
  vec2 d=skyGuideDisk(u);float r2=dot(d,d);
  return vec3(d*sqrt(max(0,2-r2)),1-r2);
}
uint skyGuideBin(vec3 local) {
  vec2 d=local.xy;float l=length(d);
  vec2 u=skyGuideSquare(l>0?d/l*sqrt(max(0,1-local.z)):vec2(0));
  uvec2 b=min(uvec2(u*8),uvec2(7));
  return b.y*8u+b.x;
}
uint skyGuideKey(Hit hit) {
  return lightGuideCellKey(floor(offsetRay(hit.p,hit.n)*skyGuideScale()),lightGuideFace(hit.n));
}
// The surface's slot of last frame's table (its base word), or ~0u.
uint skyGuideSlot(Hit hit) {
  if(frame.skyGuideRead==0)return ~0u;
  uint key=skyGuideKey(hit),base=(key&(frame.skyGuideConfig.x-1u))*skyGuideStride;
  SkyGuide guide=SkyGuide(frame.skyGuideRead);
  return guide.data[base]==key&&guide.data[base+1u]!=0u?base:~0u;
}
// The guide's PDF of a world direction (solid angle).
float skyGuidePdf(uint slot,uint face,vec3 direction) {
  vec3 local=transpose(skyGuideFrame(face))*direction;
  if(local.z<=0)return 0;
  SkyGuide guide=SkyGuide(frame.skyGuideRead);
  float share=float(guide.data[slot+4u+skyGuideBin(local)])/float(guide.data[slot+1u]);
  return ((1-skyGuideUniform)*share+skyGuideUniform/float(skyGuideBins))*float(skyGuideBins)/6.283185307;
}
// A direction from the guide: a bin by its share (or uniformly), then
// uniformly within it; `u` places it in the bin.
vec3 skyGuideDirection(uint slot,uint face,vec2 u) {
  SkyGuide guide=SkyGuide(frame.skyGuideRead);
  uint bin=0u;
  if(random()<skyGuideUniform)bin=min(uint(random()*float(skyGuideBins)),skyGuideBins-1u);
  else {
    float target=random()*float(guide.data[slot+1u]),sum=0;
    for(bin=0u;bin<skyGuideBins-1u;++bin) {
      sum+=float(guide.data[slot+4u+bin]);
      if(sum>target)break;
    }
  }
  vec2 square=(vec2(bin%8u,bin/8u)+u)/8.;
  return normalize(skyGuideFrame(face)*skyGuideHemisphere(square));
}
// Records one sky ray: `value` is its visible sky luminance over its PDF.
// The first trainer of a cell this frame carries a decayed share of last
// frame's bins, so the guide follows changes and forgets unseen cells.
void trainSkyGuide(Hit hit,vec3 direction,float value) {
  if(frame.skyGuideWrite==0||!(value>0))return;
  uint face=lightGuideFace(hit.n);
  vec3 local=transpose(skyGuideFrame(face))*direction;
  if(local.z<=0)return;
  uint key=skyGuideKey(hit),base=(key&(frame.skyGuideConfig.x-1u))*skyGuideStride;
  SkyGuide next=SkyGuide(frame.skyGuideWrite);
  uint owner=atomicCompSwap(next.data[base],0u,key);
  if(owner!=0u&&owner!=key)return; // A collision loses guidance, never energy.
  if(owner==0u&&frame.skyGuideRead!=0) {
    SkyGuide previous=SkyGuide(frame.skyGuideRead);
    if(previous.data[base]==key) {
      uint carried=0u;
      for(uint i=0u;i<skyGuideBins;++i) {
        uint kept=previous.data[base+4u+i]*3u/4u;
        if(kept!=0u){atomicAdd(next.data[base+4u+i],kept);carried+=kept;}
      }
      atomicAdd(next.data[base+1u],carried);
    }
  }
  uint amount=uint(min(value*skyGuideUnit,16383.)); // bounded: a busy cell's total stays in 32 bits
  if(amount==0u)return;
  atomicAdd(next.data[base+4u+skyGuideBin(local)],amount);
  atomicAdd(next.data[base+1u],amount);
}
