#include "eon.glsl"
mat3 shadingBasis(vec3 n) {
  vec3 t=normalize(cross(abs(n.z)<.999?vec3(0,0,1):vec3(0,1,0),n));
  return mat3(t,cross(n,t),n);
}
// Retain an albedo-independent dielectric reflection, including black plastics.
vec3 materialFactor(Hit hit) { return hit.pbr?max(hit.color.rgb,vec3(.04)):hit.color.rgb; }
vec3 fresnel(vec3 f0,float cosine) { return f0+(1-f0)*pow(clamp(1-cosine,0,1),5); }
// The specular reflectance at cosine to the microfacet normal: through a thin
// film where the surface has one (iridescence.glsl), blended by its strength.
vec3 surfaceFresnel(Hit hit,vec3 f0,float cosine) {
  vec3 f=fresnel(f0,cosine);
  return hit.film.x>0?mix(f,filmFresnel(max(cosine,1e-3),hit.film.y,hit.film.z,f0),hit.film.x):f;
}
// The reflectance seen along the view, in place of f0 where a film colours it
// (the denoiser's specular albedo guide).
vec3 filmF0(Hit hit,vec3 f0) {
  return hit.film.x>0?mix(f0,filmFresnel(max(dot(hit.n,hit.wo),1e-3),hit.film.y,hit.film.z,f0),hit.film.x):f0;
}
float ggxD(float nh,float alpha) {
  float a2=alpha*alpha,den=(1-nh*nh)+a2*nh*nh;
  return a2/(eonPi*den*den);
}
float ggxG1(float nv,float alpha) {
  return 2*nv/(nv+sqrt(alpha*alpha+(1-alpha*alpha)*nv*nv));
}
void materialLobes(Hit hit,vec3 wi,out vec3 diffuse,out vec3 specular) {
  diffuse=specular=vec3(0);
  float ni=dot(hit.n,wi),no=dot(hit.n,hit.wo);
  if(ni<=0||no<=0||dot(hit.ng,wi)<=0)return;
  if(!hit.pbr){diffuse=hit.color.rgb/eonPi;return;}
  diffuse=eonBrdfCosines(hit.color.rgb,hit.rm.x,ni,no,dot(wi,hit.wo));
  vec3 h=normalize(wi+hit.wo);
  vec3 f=surfaceFresnel(hit,mix(vec3(.04),hit.color.rgb,hit.rm.y),max(0,dot(hit.wo,h)));
  float alpha=max(.001,hit.rm.x*hit.rm.x);
  specular=f*(ggxD(max(0,dot(hit.n,h)),alpha)*ggxG1(ni,alpha)*ggxG1(no,alpha)/(4*ni*no));
  diffuse*=(1-hit.rm.y)*(1-f);
}
vec3 materialBrdf(Hit hit,vec3 wi) {
  vec3 diffuse,specular;materialLobes(hit,wi,diffuse,specular);
  return diffuse+specular;
}
// All NEE estimators previously include cos/pi. Replace that Lambert factor
// with the same BRDF used by continuation; keep the existing light PDFs.
vec3 materialResponse(Hit hit,vec3 wi) {
  if(!hit.pbr)return vec3(dot(hit.ng,wi)>0?1:0);
  return materialBrdf(hit,wi)*eonPi/max(materialFactor(hit),vec3(1e-6));
}
vec3 materialResponse(Hit hit,vec3 wi,out vec3 specular) {
  specular=vec3(0);
  if(!hit.pbr)return materialResponse(hit,wi);
  vec3 diffuse;materialLobes(hit,wi,diffuse,specular);
  vec3 scale=eonPi/max(materialFactor(hit),vec3(1e-6));
  specular*=scale;
  return diffuse*scale+specular;
}
// Diffuse share of the reflectance at this view angle (NRD sample,
// EstimateDiffuseProbability). A lobe with next to no energy (the diffuse lobe
// of a metal) gets exactly zero: its samples would all be black, which reads
// as grain on mirrors.
float diffuseShare(Hit hit) {
  if(!hit.pbr)return 1;
  vec3 f0=mix(vec3(.04),hit.color.rgb,hit.rm.y);
  float specular=dot(surfaceFresnel(hit,f0,max(0,dot(hit.n,hit.wo))),vec3(.2126,.7152,.0722));
  float diffuse=dot(hit.color.rgb,vec3(.2126,.7152,.0722))*(1-hit.rm.y)*(1-specular);
  float share=diffuse/max(diffuse+specular,1e-6);
  return share<.005?0:share>.995?1:share;
}
// Lobe selection probability: both lobes keep at least a quarter of the
// samples for the denoiser (NRD: clamp to a sane range, zero stays zero).
float specularProbability(Hit hit) {
  float share=diffuseShare(hit);
  return share==0?1:1-clamp(share,.25,.75);
}
// NRD's "magic curve": how diffuse-like a specular lobe of this roughness is.
float specularSpread(float roughness) {
  return (1-exp2(-200*roughness*roughness))*sqrt(clamp(roughness,0,1));
}
// How much reflections in this surface move like diffuse lighting (NRD
// sample, EstimateDiffuseProbability with the roughness boost).
float diffuseLikeMotion(Hit hit) {
  return hit.pbr?mix(diffuseShare(hit),1,specularSpread(hit.rm.x)):1;
}
// Heitz 2018 GGX visible-normal sampling (isotropic), followed by reflection.
vec3 sampleGgx(vec3 wo,float alpha,vec2 u) {
  vec3 v=normalize(vec3(alpha*wo.xy,wo.z));
  float lensq=dot(v.xy,v.xy);
  vec3 t1=lensq>0?vec3(-v.y,v.x,0)/sqrt(lensq):vec3(1,0,0);
  vec3 t2=cross(v,t1);
  float radius=sqrt(u.x),phi=2*eonPi*u.y;
  float x=radius*cos(phi),y=radius*sin(phi),s=.5*(1+v.z);
  y=mix(sqrt(max(0,1-x*x)),y,s);
  vec3 nh=x*t1+y*t2+sqrt(max(0,1-x*x-y*y))*v;
  vec3 h=normalize(vec3(alpha*nh.xy,max(0,nh.z)));
  return reflect(-wo,h);
}
vec3 materialSample(Hit hit,out vec3 direction,out vec3 specularWeight) {
  specularWeight=vec3(0);
  if(!hit.pbr) {
    direction=diffuseDirection(hit.n);
    return dot(direction,hit.ng)>0?hit.color.rgb:vec3(0);
  }
  mat3 basis=shadingBasis(hit.n);
  vec3 wo=transpose(basis)*hit.wo;
  if(wo.z<=0){direction=hit.n;return vec3(0);}
  float p=specularProbability(hit),choice=random();
  vec2 u=directionRandom();
  float alpha=max(.001,hit.rm.x*hit.rm.x);
  vec3 wi=choice<p?sampleGgx(wo,alpha,u):eonSample(wo,hit.rm.x,u.x,u.y).xyz;
  direction=basis*wi;
  if(wi.z<=0||dot(direction,hit.ng)<=0)return vec3(0);
  vec3 h=normalize(wi+wo);
  float specPdf=ggxD(max(0,h.z),alpha)*ggxG1(wo.z,alpha)/(4*wo.z);
  float pdf=p*specPdf+(1-p)*eonPdf(wo,wi,hit.rm.x);
  vec3 diffuse,specular;materialLobes(hit,direction,diffuse,specular);
  float scale=wi.z/max(pdf,1e-20);
  specularWeight=specular*scale;
  return (diffuse+specular)*scale;
}
vec3 materialSample(Hit hit,out vec3 direction) {
  vec3 specularWeight;
  return materialSample(hit,direction,specularWeight);
}
// One lobe of a PBR surface, sampled with that lobe's own PDF (EON for
// diffuse, GGX visible normals for specular): NRD's probabilistic lobe
// selection, where each denoised signal only receives in-lobe samples and hit
// distances. The caller divides by the selection probability.
vec3 lobeSample(Hit hit,bool diffuseLobe,out vec3 direction) {
  mat3 basis=shadingBasis(hit.n);
  vec3 wo=transpose(basis)*hit.wo;
  if(wo.z<=0){direction=hit.n;return vec3(0);}
  vec2 u=directionRandom();
  float alpha=max(.001,hit.rm.x*hit.rm.x);
  vec3 wi=diffuseLobe?eonSample(wo,hit.rm.x,u.x,u.y).xyz:sampleGgx(wo,alpha,u);
  direction=basis*wi;
  if(wi.z<=0||dot(direction,hit.ng)<=0)return vec3(0);
  vec3 diffuse,specular;materialLobes(hit,direction,diffuse,specular);
  if(diffuseLobe)return diffuse*(wi.z/max(eonPdf(wo,wi,hit.rm.x),1e-20));
  vec3 h=normalize(wi+wo);
  float pdf=ggxD(max(0,h.z),alpha)*ggxG1(wo.z,alpha)/(4*wo.z);
  return specular*(wi.z/max(pdf,1e-20));
}
