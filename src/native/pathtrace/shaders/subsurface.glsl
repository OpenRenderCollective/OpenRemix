// Subsurface scattering (RTX Remix's controls, surfaceSubsurface): Burley's
// normalized diffusion, sampled by projecting a disk around the visible point
// along its normal onto a surface with a profile. One probe ray per sample; the
// light there is then gathered as usual (direct). hit.sss: each channel's
// profile length d in scene units, and the weight.
const float sssPi=3.14159265;
// A radius drawn from the profile of mean free path d (Golubev 2018, "Sampling
// Burley's Normalized Diffusion Profiles": its CDF inverted in closed form).
float burleyRadius(float u,float d) {
  u=clamp(1-u,1e-6,1.);
  float g=1+4*u*(2*u+sqrt(1+4*u*u));
  float n=exp2(log2(g)*(-1./3.)),c=1+g*n*n+n;
  return 3*log(c/(4*u))*d;
}
// The profile at r, per unit area: (e^-r/d + e^-r/3d) / (8 pi d r), which
// integrates to one over the plane.
vec3 burleyProfile(vec3 d,float r) {
  return (exp(-r/d)+exp(-r/(3*d)))/(8*sssPi*d*r);
}
// Every crossing of a surface with a profile along the probe, one kept
// uniformly (reservoir), and the nearest one's distance. Any profile's, not
// only the visible surface's: a body split across materials has no seams.
void probeScene(accelerationStructureEXT scene,vec3 origin,vec3 direction,float extent,
                inout uint count,inout float chosen,inout float nearest,inout uint primitive,inout int surface,inout mat4x3 model) {
  rayQueryEXT query;
  rayQueryInitializeEXT(query,scene,gl_RayFlagsNoOpaqueEXT,traceMask,origin,0,direction,extent);
  while(rayQueryProceedEXT(query)) {
    if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionTriangleEXT)continue;
    int index=rayQueryGetIntersectionInstanceCustomIndexEXT(query,false)+rayQueryGetIntersectionGeometryIndexEXT(query,false);
    if(surfaces[index].subsurfaceMaps.w!=1u||surfaces[index].subsurface.w<=0)continue;
    nearest=min(nearest,rayQueryGetIntersectionTEXT(query,false));
    if(random()*float(++count)<1) {
      chosen=rayQueryGetIntersectionTEXT(query,false);
      primitive=rayQueryGetIntersectionPrimitiveIndexEXT(query,false);
      surface=index;
      model=rayQueryGetIntersectionObjectToWorldEXT(query,false);
    }
  }
}
// A thin opaque surface (RTX Remix's lobes, one drawn per sample): half the
// samples look at its far side, lit as a white diffuse surface there, tinted by
// what it passes. The rest are its own diffuse and reflection.
bool subsurfaceThinLobe(inout Hit hit,out vec3 tint,out float specular) {
  if(random()<.5) {
    tint=2*abs(hit.sss.xyz);specular=0;
    hit.n=-hit.n;hit.ng=-hit.ng;hit.wo=hit.n;hit.horizon=hit.n;hit.lift=0;
    hit.color.rgb=vec3(1);hit.rm.y=0;hit.film=vec4(0);
  } else {
    tint=vec3(hit.sss.x<0?1.:2.);specular=2;
  }
  return true;
}
// Moves `hit` to the point the profile picks: in front, or on a thin object's
// far side, whose light then glows through (specular 0: no reflection there).
// tint: the profile over the density it was drawn with (the three channels'
// profiles averaged), times the crossings the probe chose from; specular: what
// the reflection is multiplied by. False: none found; hit unchanged.
bool subsurfaceMove(inout Hit hit,out vec3 tint,out float specular) {
  tint=vec3(1);specular=1;
  if(hit.surface<0)return false;
  if(hit.sss.w<0)return subsurfaceThinLobe(hit,tint,specular);
  float largest=max(hit.sss.x,max(hit.sss.y,hit.sss.z));
  vec3 d=max(hit.sss.xyz,vec3(largest*1e-3+1e-9));
  // The profile's reach, within the material's largest sample radius.
  float rmax=min(16*largest,surfaces[hit.surface].subsurfaceThin.w*frame.subsurfaceUnits),pick=random()*3;
  float r=burleyRadius(random(),pick<1?d.x:pick<2?d.y:d.z);
  if(r>=rmax||r<=0)return false;
  float angle=6.28318530718*random();
  vec3 n=hit.ng,t=normalize(cross(abs(n.z)<.999?vec3(0,0,1):vec3(1,0,0),n)),b=cross(n,t);
  float h=sqrt(rmax*rmax-r*r);
  vec3 origin=hit.p+r*(cos(angle)*t+sin(angle)*b)+h*n;
  uint count=0u,primitive=0u;float chosen=0,nearest=2*h;int surface=hit.surface;mat4x3 model;
  if((frame.target.z&1)!=0)probeScene(staticScene,origin,-n,2*h,count,chosen,nearest,primitive,surface,model);
  if((frame.target.z&2)!=0)probeScene(dynamicScene,origin,-n,2*h,count,chosen,nearest,primitive,surface,model);
  if(count==0u)return false;
  // A crossing behind the nearest is the far side of a thin object.
  bool through=chosen>nearest+1e-3*rmax;
  if(through&&(frame.pathConfig.w&0x4000u)!=0u)return false;
  Surface s=surfaces[surface];
  s.indices=s.traceIndices;
  uint base=primitive*3;
  vec3 a=s.vertices.v[s.indices.i[base]].position.xyz;
  vec3 found=normalize(cross(model*vec4(s.vertices.v[s.indices.i[base+1]].position.xyz-a,0),
                             model*vec4(s.vertices.v[s.indices.i[base+2]].position.xyz-a,0)));
  vec3 profile=burleyProfile(d,r);
  tint=profile/max((profile.x+profile.y+profile.z)/3,1e-20)*float(count);
  hit.p=origin-n*chosen;
  // The far side is lit from behind and seen through it, its normal (and view)
  // away from this side. Winding is not trusted: the normal is turned to the
  // side it faces. The lighting horizon was the visible point's.
  found*=dot(found,n)<0==through?1.:-1.;
  if(through){hit.ng=hit.n=hit.wo=found;specular=0;}
  else hit.ng=found;
  hit.horizon=hit.n;hit.lift=0;
  return true;
}
