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
// Height-correlated Smith masking-shadowing (Heitz 2014): the separable
// product G1(i) G1(o) counts a microfacet hidden from both directions twice.
float ggxG2(float ni,float no,float alpha) {
  float a2=alpha*alpha;
  float li=sqrt(1+a2*(1/(ni*ni)-1)),lo=sqrt(1+a2*(1/(no*no)-1));
  return 2/(li+lo);
}
// Directional albedo of single-scattering GGX (F = 1, height-correlated
// Smith), integrated with visible-normal sampling: rows perceptual roughness
// 0..1, columns cos(view) 0..1, 16 steps each.
const float ggxAlbedoTable[256]=float[](
  0.8922,0.9999,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,
  0.9568,0.9975,0.9994,0.9997,0.9999,0.9999,0.9999,0.9999,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,1.0000,
  0.9883,0.9602,0.9892,0.9952,0.9973,0.9982,0.9988,0.9991,0.9992,0.9994,0.9995,0.9995,0.9996,0.9996,0.9996,0.9997,
  0.9945,0.9008,0.9512,0.9751,0.9854,0.9905,0.9933,0.9949,0.9960,0.9966,0.9970,0.9975,0.9978,0.9980,0.9982,0.9983,
  0.9967,0.8886,0.9048,0.9353,0.9565,0.9698,0.9775,0.9828,0.9863,0.9885,0.9904,0.9915,0.9925,0.9933,0.9939,0.9943,
  0.9975,0.9001,0.8811,0.8954,0.9163,0.9341,0.9479,0.9579,0.9655,0.9709,0.9751,0.9782,0.9807,0.9824,0.9840,0.9853,
  0.9978,0.9112,0.8752,0.8699,0.8791,0.8931,0.9076,0.9208,0.9315,0.9403,0.9476,0.9538,0.9580,0.9620,0.9651,0.9679,
  0.9977,0.9172,0.8735,0.8545,0.8505,0.8561,0.8656,0.8760,0.8869,0.8969,0.9061,0.9141,0.9217,0.9281,0.9326,0.9373,
  0.9975,0.9163,0.8697,0.8420,0.8277,0.8222,0.8241,0.8292,0.8367,0.8450,0.8531,0.8617,0.8698,0.8765,0.8836,0.8906,
  0.9971,0.9105,0.8593,0.8255,0.8041,0.7903,0.7835,0.7820,0.7829,0.7871,0.7914,0.7973,0.8047,0.8111,0.8179,0.8245,
  0.9966,0.9005,0.8448,0.8050,0.7764,0.7566,0.7429,0.7335,0.7284,0.7256,0.7257,0.7267,0.7300,0.7341,0.7380,0.7423,
  0.9960,0.8875,0.8246,0.7790,0.7458,0.7186,0.6988,0.6831,0.6714,0.6622,0.6567,0.6535,0.6509,0.6499,0.6494,0.6505,
  0.9954,0.8723,0.8010,0.7484,0.7088,0.6772,0.6518,0.6302,0.6139,0.5988,0.5866,0.5781,0.5703,0.5645,0.5582,0.5556,
  0.9947,0.8540,0.7733,0.7154,0.6691,0.6322,0.6020,0.5752,0.5536,0.5351,0.5178,0.5039,0.4916,0.4801,0.4715,0.4630,
  0.9939,0.8355,0.7451,0.6793,0.6273,0.5854,0.5496,0.5198,0.4944,0.4716,0.4513,0.4345,0.4182,0.4035,0.3903,0.3794,
  0.9932,0.8153,0.7150,0.6411,0.5843,0.5370,0.4986,0.4653,0.4366,0.4117,0.3896,0.3696,0.3515,0.3350,0.3205,0.3065);
float ggxAlbedo(float roughness,float no) {
  vec2 at=vec2(clamp(roughness,0.,1.),clamp(no,1.0/15,1.))*15;
  ivec2 i=min(ivec2(at),ivec2(14));vec2 f=at-vec2(i);
  float a=mix(ggxAlbedoTable[i.x*16+i.y],ggxAlbedoTable[i.x*16+i.y+1],f.y);
  float b=mix(ggxAlbedoTable[i.x*16+16+i.y],ggxAlbedoTable[i.x*16+17+i.y],f.y);
  return mix(a,b,f.x);
}
// The light single scattering loses to masking, returned by scattering again
// (Turquin 2019; RTXPT): rough metals otherwise lose up to two thirds of it.
vec3 ggxMultiScatter(vec3 f0,float roughness,float no) {
  float e=max(ggxAlbedo(roughness,no),.05);
  return 1+f0*(1-e)/e;
}
// The diffuse lobe ends at the lighting horizon (terminatorLift: the smooth
// surface's on lifted triangles, else the triangle's), the specular one at
// the triangle's.
void materialLobes(Hit hit,vec3 wi,out vec3 diffuse,out vec3 specular) {
  diffuse=specular=vec3(0);
  float ni=dot(hit.n,wi),no=dot(hit.n,hit.wo);
  if(ni<=0||no<=0||dot(hit.horizon,wi)<=0)return;
  if(!hit.pbr){diffuse=hit.color.rgb/eonPi;return;}
  diffuse=eonBrdfCosines(hit.color.rgb,hit.rm.x,ni,no,dot(wi,hit.wo));
  vec3 h=normalize(wi+hit.wo),f0=hitF0(hit);
  vec3 f=surfaceFresnel(hit,f0,max(0,dot(hit.wo,h)));
  float alpha=max(.001,hit.rm.x*hit.rm.x);
  if(dot(hit.ng,wi)>0)specular=f*ggxMultiScatter(f0,hit.rm.x,no)*(ggxD(max(0,dot(hit.n,h)),alpha)*ggxG2(ni,no,alpha)/(4*ni*no));
  diffuse*=(1-hit.rm.y)*(1-f);
}
vec3 materialBrdf(Hit hit,vec3 wi) {
  vec3 diffuse,specular;materialLobes(hit,wi,diffuse,specular);
  return diffuse+specular;
}
// Softening of the shadow terminator on light sampling's diffuse lobe
// (dxvk-remix softenShadowTerminator, after "Predictable and Targeted
// Softening of the Shadow Terminator"): light within about 3 degrees of the
// lighting horizon (materialLobes) fades out, within about 9 where the shading normal (a
// normal map, smooth normals) departs from the triangle's, so the abrupt cut
// where the triangle turns away is a gradient. A band that thin takes next
// to no light, and the triangle normal's differences between neighbours do
// not show (a broad fade by it put steps at triangle edges). 1 with
// rtx.native.pathtrace.shadowTerminatorSoften off (frame.localProbeParams.y).
// Sampled bounce directions keep the full BRDF.
float terminatorSoften(Hit hit,vec3 wi) {
  if(frame.localProbeParams.y==0)return 1;
  float threshold=mix(sin(.15),sin(.05),clamp(dot(hit.n,hit.horizon),0.,1.));
  float shadowing=clamp(dot(hit.horizon,wi)/threshold,0.,1.);
  return shadowing*shadowing*(3-2*shadowing);
}
// Emissive surfaces in the specular lobe of a smooth PBR surface (roughness
// below rtx.native.pathtrace.emissiveSpecularRoughness, frame.localProbeParams.z;
// 0 off) are found by its sampled path directions instead of light sampling:
// a uniform point on an emitter rarely lands in a narrow GGX lobe, a
// reflection of a screen or panel stayed noisy. Light sampling keeps the
// diffuse lobe and every sphere light; the path adds the emission its next
// hit shows, weighted by the scattering's specular share (continuation). Set
// by callers only where that path follows the vertex (the last vertex of a
// path, and cache training, keep full light sampling).
bool emissiveSpecularFromPaths=false;
bool pathEmissiveSpecular(Hit hit) {
  return emissiveSpecularFromPaths&&hit.pbr&&hit.rm.x<frame.localProbeParams.z;
}
// All NEE estimators previously include cos/pi. Replace that Lambert factor
// with the same BRDF used by continuation; keep the existing light PDFs.
vec3 materialResponse(Hit hit,vec3 wi,out vec3 specular) {
  specular=vec3(0);
  if(!hit.pbr)return vec3(dot(hit.horizon,wi)>0?terminatorSoften(hit,wi):0);
  vec3 diffuse;materialLobes(hit,wi,diffuse,specular);
  vec3 scale=eonPi/max(materialFactor(hit),vec3(1e-6));
  specular*=scale;
  return diffuse*(scale*terminatorSoften(hit,wi))+specular;
}
vec3 materialResponse(Hit hit,vec3 wi) {
  vec3 specular;return materialResponse(hit,wi,specular);
}
// Diffuse share of the reflectance at this view angle (NRD sample,
// EstimateDiffuseProbability). A lobe with next to no energy (the diffuse lobe
// of a metal) gets exactly zero: its samples would all be black, which reads
// as grain on mirrors.
float diffuseShare(Hit hit) {
  if(!hit.pbr)return 1;
  vec3 f0=hitF0(hit);
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
// The PDF of the last direction materialSample or lobeSample drew (0: none),
// which widens a path's ray cone.
float samplePdf=0;
// A direction drawn about the shading normal (a normal map, smooth normals on
// a low-poly mesh) that points below the triangle, mirrored across its plane:
// dropping it left creases black in indirect light (a character's nostrils,
// collar and eye sockets), up to half the samples where the normals bend.
vec3 aboveSurface(vec3 direction,vec3 ng) {
  float below=dot(direction,ng);
  return below<0?direction-2*below*ng:direction;
}
vec3 materialSample(Hit hit,out vec3 direction,out vec3 specularWeight) {
  specularWeight=vec3(0);samplePdf=0;
  if(!hit.pbr) {
    direction=aboveSurface(diffuseDirection(hit.n),hit.ng);
    samplePdf=max(dot(direction,hit.n),0.)/eonPi;
    return dot(direction,hit.ng)>0?hit.color.rgb:vec3(0);
  }
  mat3 basis=shadingBasis(hit.n);
  vec3 wo=transpose(basis)*hit.wo;
  if(wo.z<=0){direction=hit.n;return vec3(0);}
  float p=specularProbability(hit),choice=random();
  vec2 u=directionRandom();
  float alpha=max(.001,hit.rm.x*hit.rm.x);
  vec3 wi=choice<p?sampleGgx(wo,alpha,u):eonSample(wo,hit.rm.x,u.x,u.y).xyz;
  direction=aboveSurface(basis*wi,hit.ng);
  if(wi.z<=0||dot(direction,hit.ng)<=0)return vec3(0);
  vec3 h=normalize(wi+wo);
  float specPdf=ggxD(max(0,h.z),alpha)*ggxG1(wo.z,alpha)/(4*wo.z);
  float pdf=p*specPdf+(1-p)*eonPdf(wo,wi,hit.rm.x);
  samplePdf=pdf;
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
  samplePdf=0;
  mat3 basis=shadingBasis(hit.n);
  vec3 wo=transpose(basis)*hit.wo;
  if(wo.z<=0){direction=hit.n;return vec3(0);}
  vec2 u=directionRandom();
  float alpha=max(.001,hit.rm.x*hit.rm.x);
  vec3 wi=diffuseLobe?eonSample(wo,hit.rm.x,u.x,u.y).xyz:sampleGgx(wo,alpha,u);
  direction=aboveSurface(basis*wi,hit.ng);
  if(wi.z<=0||dot(direction,hit.ng)<=0)return vec3(0);
  vec3 diffuse,specular;materialLobes(hit,direction,diffuse,specular);
  if(diffuseLobe){samplePdf=eonPdf(wo,wi,hit.rm.x);return diffuse*(wi.z/max(samplePdf,1e-20));}
  vec3 h=normalize(wi+wo);
  float pdf=ggxD(max(0,h.z),alpha)*ggxG1(wo.z,alpha)/(4*wo.z);
  samplePdf=pdf;
  return specular*(wi.z/max(pdf,1e-20));
}
