// Thin-film iridescence (remixapi_MaterialIridescenceEXT; frame_tables.h
// filmFields). The reflectance of a film over the surface is the Airy sum of
// its interreflections, integrated against the CIE colour matching functions
// in closed form (Belcour and Barla 2017, "A Practical Extension to Microfacet
// Theory for the Modeling of Varying Iridescence"): the colours of a whole
// spectrum, not of three wavelengths, so thick films fade to pastel instead
// of aliasing.
const float filmPi=3.14159265;
// CIE XYZ to linear Rec. 709 (columns).
const mat3 filmXyzToRgb=mat3(3.2404542,-.9692660,.0556434,-1.5371385,1.8760108,-.2040259,-.4985314,.0415560,1.0572252);
// The Fourier transform of the colour matching functions (Gaussian fits) at
// an optical path difference in nm, phase shifted per channel: one order of
// interference, as linear RGB.
vec3 filmSensitivity(float opd,vec3 shift) {
  float phase=2*filmPi*opd*1e-9;
  const vec3 value=vec3(5.4856e-13,4.4201e-13,5.2481e-13);
  const vec3 position=vec3(1.6810e+06,1.7953e+06,2.2084e+06);
  const vec3 variance=vec3(4.3278e+09,9.3046e+09,6.6121e+09);
  vec3 xyz=value*sqrt(2*filmPi*variance)*cos(position*phase+shift)*exp(-phase*phase*variance);
  xyz.x+=9.7470e-14*sqrt(2*filmPi*4.5282e+09)*cos(2.2399e+06*phase+shift.x)*exp(-4.5282e+09*phase*phase);
  return filmXyzToRgb*(xyz/1.0685e-7);
}
float filmSchlick(float f0,float cosine) { float m=clamp(1-cosine,0,1),m2=m*m; return f0+(1-f0)*m2*m2*m; }
vec3 filmSchlick(vec3 f0,float cosine) { float m=clamp(1-cosine,0,1),m2=m*m; return f0+(1-f0)*m2*m2*m; }
// The reflectance at cosine (of the angle to the microfacet normal) of a film
// of index eta and thickness nm over a surface of normal-incidence
// reflectance f0, from air.
vec3 filmFresnel(float cosine,float eta,float thickness,vec3 f0) {
  // A vanishing film fades into the bare surface, not into a 1.5 coat.
  eta=mix(1,eta,smoothstep(0,30,thickness));
  float sin2=(1-cosine*cosine)/(eta*eta);
  if(sin2>=1)return vec3(1);
  float cosine2=sqrt(1-sin2);
  // Air to film.
  float r0=(eta-1)/(eta+1);
  float r12=filmSchlick(r0*r0,cosine),t121=1-r12;
  // Film to surface: the surface's index from its reflectance (a dielectric
  // reading; metals keep their colour through f0).
  vec3 root=sqrt(clamp(f0,0,.9999));
  vec3 ior=(1+root)/(1-root);
  vec3 r1=(ior-eta)/(ior+eta);
  vec3 r23=filmSchlick(r1*r1,cosine2);
  // Reflection off a denser medium shifts the phase by pi.
  vec3 phase=vec3(filmPi)+mix(vec3(0),vec3(filmPi),lessThan(ior,vec3(eta)));
  float opd=2*eta*thickness*cosine2;
  vec3 r123=clamp(r12*r23,1e-5,.9999),root123=sqrt(r123);
  vec3 rs=t121*t121*r23/(1-r123);
  vec3 reflectance=r12+rs,c=rs-t121;
  for(int m=1;m<=2;++m) {
    c*=root123;
    reflectance+=c*2*filmSensitivity(float(m)*opd,float(m)*phase);
  }
  return max(reflectance,vec3(0));
}
// Gradient noise in -1..1 for the film's flow.
vec3 filmHash(vec3 p) {
  p=fract(p*vec3(.1031,.1030,.0973));
  p+=dot(p,p.yxz+33.33);
  return fract((p.xxy+p.yxx)*p.zyx)*2-1;
}
float filmNoise(vec3 p) {
  vec3 i=floor(p),f=fract(p),u=f*f*(3-2*f);
  float n[8];
  for(int k=0;k<8;++k) {
    vec3 o=vec3(k&1,(k>>1)&1,k>>2);
    n[k]=dot(filmHash(i+o),f-o);
  }
  return 1.7*mix(mix(mix(n[0],n[1],u.x),mix(n[2],n[3],u.x),u.y),mix(mix(n[4],n[5],u.x),mix(n[6],n[7],u.x),u.y),u.z);
}
// Swirls in -1..1 drifting with time: one domain warp, then three octaves.
float filmFlow(vec3 p,float t) {
  vec3 q=p+vec3(0,0,t);
  vec3 warp=vec3(filmNoise(q+vec3(1.7,9.2,0)),filmNoise(q+vec3(8.3,2.8,5.1)),filmNoise(q+vec3(4.1,6.5,3.3)));
  q=p+1.5*warp+t*vec3(.31,.17,.11);
  return clamp(.57*filmNoise(q)+.29*filmNoise(q*2.03)+.14*filmNoise(q*4.01),-1,1);
}
// A surface's film at a point: strength (0: none), film index, thickness in
// nm and structural colour. local: the point in object space; alpha: the
// albedo's (the thickness when the film reads it).
vec4 surfaceFilm(Surface s,vec2 uv,vec3 local,float alpha) {
  if(!filmed(s))return vec4(0);
  uint map=s.filmMaps.w&0x7fffffffu;
  float thickness=(s.filmMaps.w&0x80000000u)!=0?alpha*1500:s.film.z;
  if(map!=0) {
    vec2 range=unpackHalf2x16(s.filmMaps.x);
    thickness=mix(range.x,range.y,materialTexel(s,map-1,uv).r);
  }
  vec2 flow=unpackHalf2x16(s.filmMaps.y);
  if(flow.x>0)
    thickness+=flow.x*filmFlow(local/max(flow.y,1e-4),frame.animationTime*uintBitsToFloat(s.filmMaps.z));
  return vec4(s.film.x,s.film.y,clamp(thickness,0,2000),s.film.w);
}
// What structural colour multiplies a filmed surface's albedo by, seen at
// cosine to its normal: the film's hue at full brightness.
vec3 filmTint(vec4 film,vec3 f0,float cosine) {
  if(film.x<=0||film.w<=0)return vec3(1);
  vec3 c=filmFresnel(max(cosine,1e-3),film.y,film.z,f0);
  c/=max(max(c.r,max(c.g,c.b)),1e-4);
  return mix(vec3(1),c,film.w*film.x);
}
