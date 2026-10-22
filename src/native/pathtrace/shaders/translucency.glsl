// The first surface owns these guides before opaque traversal reuses the raster
// previous-position image as emission scratch. Only a single visible sheet is
// filtered; layered pixels keep the bounded front-to-back reference path.
void frontGuides(Hit hit,ivec2 pixel) {
  if(temporal.translucency.x==0)return;
  bool valid=hit.t<1e30&&hit.blended;
  float z=valid?abs((temporal.view*vec4(hit.p,1)).z):1e10;
  vec4 motion=vec4(0);
  if(valid&&temporal.flags.x==0) {
    vec4 clip=frame.camera*vec4(hit.p,1),previous=temporal.previousCamera*vec4(hit.previousPosition,1);
    if(clip.w>0&&previous.w>0)
      motion=vec4((previous.xy/previous.w-clip.xy/clip.w)*.5*vec2(frame.target.xy),
                  abs((temporal.previousView*vec4(hit.previousPosition,1)).z)-z,1);
  }
  vec3 diffuseFactor=valid?max(hit.color.rgb,vec3(.001)):vec3(1),specularFactor=vec3(1);
  if(valid&&hit.pbr)
    NRD_MaterialFactors(hit.n,hit.wo,materialDiffuseAlbedo(hit),
      materialF0(hit),hit.rm.x,diffuseFactor,specularFactor);
  imageStore(frontDepth,pixel,vec4(z));
  imageStore(frontNormal,pixel,NRD_FrontEnd_PackNormalAndRoughness(valid?hit.n:vec3(0,0,1),
    valid&&hit.pbr?hit.rm.x:1,valid&&hit.pbr?(hit.rm.y>.5?2:1):0));
  imageStore(frontMotion,pixel,motion);
  imageStore(frontConfidence,pixel,vec4(motion.w));
  imageStore(frontAlbedo,pixel,vec4(diffuseFactor,valid?hit.color.a:0));
  imageStore(frontSpecularFactor,pixel,vec4(specularFactor,0));
  imageStore(frontDiffuse,pixel,vec4(0));
  imageStore(frontSpecular,pixel,vec4(0));
}

void frontRadiance(Hit hit,ivec2 pixel,vec3 radiance,vec3 specular,float distance) {
  if(temporal.translucency.x==0)return;
  float z=imageLoad(frontDepth,pixel).x;
  vec3 diffuseFactor=imageLoad(frontAlbedo,pixel).rgb;
  vec3 specularFactor=imageLoad(frontSpecularFactor,pixel).rgb;
  // Both lobes contain valid estimates at every foreground pixel, including
  // direct-only lighting. Zero is NRD's missing-sample sentinel, so absence of
  // a continuation hit uses the environment distance instead.
  distance=distance>0?distance:65504;
  imageStore(frontDiffuse,pixel,REBLUR_FrontEnd_PackRadianceAndNormHitDist(
    max(radiance-specular,vec3(0))/diffuseFactor,
    REBLUR_FrontEnd_GetNormHitDist(distance,z,temporal.hitDistance.xyz,1),true));
  imageStore(frontSpecularFactor,pixel,vec4(specularFactor,distance>0?min(distance,65504):65504));
  imageStore(frontSpecular,pixel,REBLUR_FrontEnd_PackRadianceAndNormHitDist(
    specular/specularFactor,
    REBLUR_FrontEnd_GetNormHitDist(distance,z,temporal.hitDistance.xyz,hit.pbr?hit.rm.x:1),true));
}
