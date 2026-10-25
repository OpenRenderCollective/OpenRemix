// Presampled light pools per world cell (ReGIR: RT Gems II ch. 23; RTXDI).
// Shading points register the (jittered) cell they look up; the next frame's
// presample pass fills each registered cell with K light-tree samples drawn
// for the whole cell, and a candidate becomes one slot load instead of a
// root-to-leaf descent. Each slot stores the leaf and its exact tree PMF for
// the cell, so a uniformly chosen slot is a sample of that PMF. The cell
// weights are conservative over the cell box (expanded by the lookup jitter)
// and ignore the receiver normal, so every light that can reach a point
// looking up the cell has a nonzero PMF: estimators stay unbiased, and cells
// not presampled yet fall back to the tree.
layout(buffer_reference,std430,buffer_reference_align=16) buffer RegirCells { uvec4 data[]; };
layout(buffer_reference,std430,buffer_reference_align=8) buffer RegirSlots { uvec2 data[]; };
const uint regirProbes=8u,regirNone=0xffffffffu;
uint regirKey(ivec3 cell) {
  return max(1u,hash(hash(uint(cell.x))^hash(uint(cell.y)+0x9e3779b9u)^hash(uint(cell.z)+0x85ebca6bu)));
}
// Stochastic lookup: the cell under the point offset by up to half a cell.
ivec3 regirCell(vec3 p,inout uint state) {
  vec3 jitter=vec3(lightRandom(state),lightRandom(state),lightRandom(state))-.5;
  return ivec3(floor(p*uintBitsToFloat(frame.regirConfig.z)+jitter));
}
// First slot of the cell's pool, or regirNone when it was not presampled.
uint regirFind(ivec3 cell) {
  if(frame.regirRead==0)return regirNone;
  RegirCells cells=RegirCells(frame.regirRead);
  uint key=regirKey(cell),mask=frame.regirConfig.x-1;
  for(uint i=0;i<regirProbes;++i) {
    uvec4 entry=cells.data[(key+i)&mask];
    if(entry.w==key&&ivec3(entry.xyz)==cell)return ((key+i)&mask)*frame.regirConfig.y;
    if(entry.w==0)break;
  }
  return regirNone;
}
// Request the cell's pool for the next frame (a full table only loses pools).
void regirRegister(ivec3 cell) {
  RegirCells cells=RegirCells(frame.regirWrite);
  uint key=regirKey(cell),mask=frame.regirConfig.x-1;
  for(uint i=0;i<regirProbes;++i) {
    uint slot=(key+i)&mask;
    uint owner=cells.data[slot].w;
    if(owner==0)owner=atomicCompSwap(cells.data[slot].w,0u,key);
    if(owner==0){cells.data[slot].xyz=uvec3(cell);return;}
    if(owner==key)return;
  }
}
// A cell's pool for this point: looks up and registers the jittered cell.
uint regirPool(vec3 p,uint state) {
  if(frame.regirConfig.w==0)return regirNone;
  uint lookup=hash(state^0x52656749u);
  ivec3 cell=regirCell(p,lookup);
  regirRegister(cell);
  return regirFind(cell);
}
// Slot for a variate u in [0,1): slots are stratified over the tree in order.
bool regirSample(uint pool,float u,out uint leaf,out float pmf) {
  uvec2 slot=RegirSlots(frame.regirSlots).data[pool+min(uint(u*float(frame.regirConfig.y)),frame.regirConfig.y-1)];
  leaf=slot.x;pmf=uintBitsToFloat(slot.y);
  return leaf!=regirNone&&pmf>0;
}
// Light-tree weight for any point in a box, any normal: emitter power over
// the squared box-to-cluster distance, with the estimators' regularization.
float regirWeight(LightNode node,vec3 lo,vec3 hi) {
  vec3 halfSize=.5*(node.maximum.xyz-node.minimumPower.xyz);
  vec3 delta=.5*(node.maximum.xyz+node.minimumPower.xyz)-.5*(lo+hi);
  vec3 nearest=max(abs(delta)-halfSize-.5*(hi-lo),vec3(0));
  float near2=dot(nearest,nearest),radius2=dot(halfSize,halfSize);
  float distance2=max(sqrt(near2*dot(delta,delta)),.01*radius2);
  return localFalloff(near2)*node.minimumPower.w/max(max(distance2,node.maximum.w),1e-12);
}
#ifdef REGIR_PRESAMPLE_PASS
void regirPresample(uint index) {
  uint slots=frame.regirConfig.y,entry=index/slots,slot=index%slots;
  if(entry>=frame.regirConfig.x||frame.regirRead==0)return;
  uvec4 cell=RegirCells(frame.regirRead).data[entry];
  if(cell.w==0)return;
  // The cell box plus the lookup jitter (half a cell on every side).
  float size=1/uintBitsToFloat(frame.regirConfig.z);
  vec3 lo=(vec3(ivec3(cell.xyz))-.5)*size,hi=(vec3(ivec3(cell.xyz))+1.5)*size;
  LightNodes nodes=LightNodes(frame.lightNodes);
  uint state=hash(index^hash(frame.config.z^0x70726573u));
  float u=(float(slot)+lightRandom(state))/float(slots),pmf=1;
  uint node=0;
  while(nodes.data[node].links.w>1) {
    uvec4 links=nodes.data[node].links;
    float a=regirWeight(nodes.data[links.x],lo,hi),b=regirWeight(nodes.data[links.y],lo,hi);
    if(a==0&&b==0){pmf=0;break;}
    float p=lightSplit(a,b);
    if(u<p){node=links.x;pmf*=p;u/=p;}else{node=links.y;pmf*=1-p;u=(u-p)/(1-p);}
    u=min(u,.99999994);
  }
  RegirSlots(frame.regirSlots).data[index]=uvec2(pmf>0?node:regirNone,floatBitsToUint(pmf));
}
#endif
