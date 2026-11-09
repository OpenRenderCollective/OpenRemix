// Opt-in structural counters, sampled at one in 64 pixel invocations. Timing
// comparisons must run without this instrumentation. A 64-word block per
// pass family: opaque (and every pass not named here), split direct,
// transparency, the separate indirect pass (its wavefront kernels too), the
// SHARC update. Confidence excluded.
layout(buffer_reference,std430,buffer_reference_align=4) buffer RayCounters { uint counts[]; };
void profileRay(uint counter) {
#if !defined(CONFIDENCE_PASS)
  if(!enableRayProfile||(frame.reuseConfig.z==0&&frame.reuseConfig.w==0))return;
  if((hash(uint(shadingPixel.x)+uint(shadingPixel.y)*frame.target.x)&63u)!=0)return;
#ifdef SPLIT_LIGHTING_PASS
  counter+=64;
#elif defined(TRANSPARENCY_PASS)
  counter+=128;
#elif defined(INDIRECT_PASS)
  counter+=192;
#elif defined(SHARC_UPDATE_PASS)
  counter+=256;
#endif
  uint64_t address=uint64_t(frame.reuseConfig.z)|(uint64_t(frame.reuseConfig.w)<<32);
  atomicAdd(RayCounters(address).counts[counter],1u);
#endif
}
// Per-pass path events occupy words 24..36 of each 64-word counter block.
// Depth counts are segments started, including misses; sheets do not add depth.
void profilePath(uint event) { profileRay(24u+event); }
// alpha_texture records texture-dependent alpha tests, not hardware fetches.
void profileAlpha(Surface s,uint base) {
  if(s.state.y!=0&&(s.state.y!=7||coverageBlended(s))&&(s.state.w&1)==0) {
    profileRay(base+2);
    uint op=(s.state.x>>6)&3u,a=(s.state.x>>8)&3u,b=(s.state.x>>10)&3u;
    if((op!=2&&a==1)||(op!=1&&b==1))profileRay(base+6);
  }
}
