// Current-frame opaque radiance is available before translucent continuations.
// This is a filtered path-tail approximation, restricted to matching diffuse or
// rough dielectric endpoints. Failed checks leave the original tracer/RNG intact.
bool translucentRadiance(Hit hit,out vec3 radiance) {
#if defined(TEMPORAL) && defined(SHEET_LIGHTING_PASS)
  if(temporal.translucency.y==0||hit.blended||hit.t==1e30)return false;
  if(hit.pbr&&(hit.rm.x<.8||hit.rm.y>.05))return false;
  vec4 clip=frame.camera*vec4(hit.p,1);
  if(clip.w<=0)return false;
  vec2 coordinate=(clip.xy/clip.w*.5+.5)*vec2(frame.target.xy)-frame.jitter.xy;
  ivec2 pixel=ivec2(floor(coordinate));
  if(any(lessThan(pixel,ivec2(1)))||any(greaterThanEqual(pixel,ivec2(frame.target.xy)-1)))return false;
  float depth=imageLoad(guideDepth,pixel).x;
  float z=abs((temporal.view*vec4(hit.p,1)).z);
  if(depth>500000||abs(depth-z)>max(.001*z,.002))return false;
  float material;
  vec4 normal=NRD_FrontEnd_UnpackNormalAndRoughness(imageLoad(guideNormal,pixel),material);
  if(dot(normal.xyz,hit.n)<.999||dot(normal.xyz,hit.ng)<.98||
     abs(normal.w-(hit.pbr?hit.rm.x:1))>.025||abs(material-(hit.pbr?0:2))>.1)return false;
  vec3 diffuseFactor=hit.color.rgb,specularFactor=vec3(1);
  if(hit.pbr) {
    vec2 ndc=(vec2(pixel)+.5+frame.jitter.xy)/vec2(frame.target.xy)*2-1;
    vec4 nearPoint=frame.inverseCamera*vec4(ndc,0,1);
    vec4 farPoint=frame.inverseCamera*vec4(ndc,.99999,1);
    vec3 direction=normalize(farPoint.xyz/farPoint.w-nearPoint.xyz/nearPoint.w);
    // EON and GGX are view-dependent even at high roughness.
    if(dot(-direction,hit.wo)<.995)return false;
    NRD_MaterialFactors(hit.n,-direction,materialDiffuseAlbedo(hit),
      materialF0(hit),hit.rm.x,diffuseFactor,specularFactor);
    if(any(greaterThan(abs(imageLoad(pbrSpecularFactor,pixel).rgb-specularFactor),vec3(.005))))return false;
  }
  vec4 albedo=imageLoad(guideAlbedo,pixel);
  if(albedo.a<.5||(albedo.a>1.5)!=hit.pbr||
     any(greaterThan(abs(albedo.rgb-diffuseFactor),vec3(.005))))return false;
  radiance=imageLoad(composed,pixel).rgb;
  return !any(isnan(radiance))&&!any(isinf(radiance))&&!any(lessThan(radiance,vec3(0)));
#else
  return false;
#endif
}
