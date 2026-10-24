// Volumetrics (volume.cpp; rtx.native.pathtrace.volumetrics). The light a
// participating medium scatters towards any direction is held in world
// space: nested cubic grids of cells around the camera, each level's cells
// twice the size of the one before, addressed by the cell's world coordinate
// modulo the grid size so a moving camera only replaces the cells that enter.
// A cell stores the scattered radiance per unit of scattering albedo
// (isotropic phase function: the incident light's mean over the sphere) and,
// in w, how many samples it holds (0: none yet). One more cell after the
// grids holds the light of the medium beyond them (volumeFar), and a second
// set of cells the environment's share of each, averaged over sixteen times
// as many samples.
//
// The update pass gives a share of the cells one sample each frame: the sun,
// two directions of the environment and the local lights (volumeLocal), each
// with one visibility ray, at a random point of the cell. Camera rays march the grids
// (volumeMarch) when the transparency passes compose the frame.
layout(buffer_reference,std430,buffer_reference_align=16) buffer VolumeCells { vec4 data[]; };
float volumeCell(uint level) { return frame.volumeParams.x*float(1u<<level); }
// The lowest cell coordinate of a level's window around the eye.
ivec3 volumeOrigin(vec3 eye,uint level) {
  return ivec3(floor(eye/volumeCell(level)))-int(frame.volumeConfig.y>>1);
}
ivec3 volumeWrap(ivec3 cell) {
  float n=float(frame.volumeConfig.y);
  return cell-ivec3(floor(vec3(cell)/n)*n);
}
uint volumeIndex(uint level,ivec3 cell) {
  uint n=frame.volumeConfig.y;uvec3 m=uvec3(volumeWrap(cell));
  return ((level*n+m.z)*n+m.y)*n+m.x;
}
// Extinction per world unit at a point.
float volumeDensity(vec3 p) {
  float density=frame.volumeMedium.w;
  if(frame.volumeHeight.w>0)
    density*=exp(-max(dot(frame.volumeHeight.xyz,p)-frame.volumeParams.w,0)*frame.volumeHeight.w);
  return density;
}
// A level's cells interpolated at a point (clamped to the level's window);
// w: the weight of the cells that hold samples.
vec4 volumeFetch(uint level,vec3 p,ivec3 origin,uint first) {
  int n=int(frame.volumeConfig.y);
  vec3 q=clamp(p/volumeCell(level)-.5,vec3(origin),vec3(origin+n-1));
  ivec3 base=min(ivec3(floor(q)),origin+n-2);
  vec3 f=q-vec3(base);
  VolumeCells cells=VolumeCells(frame.volumeGrid);
  vec4 sum=vec4(0);
  for(int corner=0;corner<8;++corner) {
    ivec3 offset=ivec3(corner&1,(corner>>1)&1,corner>>2);
    vec3 w3=mix(1-f,f,vec3(offset));
    vec4 cell=cells.data[first+volumeIndex(level,base+offset)];
    if(cell.w>0)sum+=vec4(cell.rgb,1)*(w3.x*w3.y*w3.z);
  }
  return sum;
}
vec4 volumeFetch(uint level,vec3 p,ivec3 origin) { return volumeFetch(level,p,origin,0u); }
// The cells are followed by the one for the medium beyond the grids, then by
// every cell's share of the environment (volumeUpdate) and the far cell's.
uint volumeSkyFirst() {
  uint n=frame.volumeConfig.y;
  return n*n*n*frame.volumeConfig.z+1u;
}
// Past the last level the medium holds the open sky's light: the sun and the
// environment's mean, unshadowed, in one cell after the grids. (The last
// level's border cells stretched outwards instead drew that level's window
// on the sky: a cube, its edges and a star at each corner.)
vec4 volumeFar() {
  uint n=frame.volumeConfig.y;
  return VolumeCells(frame.volumeGrid).data[n*n*n*frame.volumeConfig.z];
}
// The scattered radiance at a point: the finest level that holds it, faded
// into the next one over the window's outer two cells.
vec3 volumeLookup(vec3 p) {
  uint levels=frame.volumeConfig.z;int n=int(frame.volumeConfig.y);
  for(uint level=0;level<levels;++level) {
    ivec3 origin=volumeOrigin(frame.volumeEye.xyz,level);
    vec3 q=p/volumeCell(level)-.5;
    vec3 inside=min(q-vec3(origin),vec3(origin+n-1)-q);
    float border=min(inside.x,min(inside.y,inside.z));
    bool last=level+1==levels;
    if(border<0&&!last)continue;
    if(border<=0)break;
    vec4 value=volumeFetch(level,p,origin);
    vec3 result=value.w>0?value.rgb/value.w:vec3(0);
    float weight=clamp(border*.5,0,1)*(value.w>0?1.:0.);
    if(weight<1) {
      vec4 outer=last?volumeFar():volumeFetch(level+1,p,volumeOrigin(frame.volumeEye.xyz,level+1));
      if(last)outer.w=1;
      if(outer.w>0)result=mix(outer.rgb/outer.w,result,weight);
    }
    return result;
  }
  return volumeFar().rgb;
}
// What the medium between the camera and `far` along a ray adds to the
// radiance behind it and how much of that it lets through. Sample distances
// grow quadratically (the near medium counts most) and are jittered.
void volumeMarch(vec3 origin,vec3 direction,float far,out vec3 inscatter,out float transmittance) {
  inscatter=vec3(0);transmittance=1;
  if(frame.volumeConfig.x==0u||frame.volumeGrid==0)return;
  uint steps=max(frame.volumeFrame.z,1u);
  float reach=min(far,frame.volumeParams.y),jitter=random(),near=0;
  for(uint i=0;i<steps;++i) {
    float u=float(i+1)/float(steps),next=reach*u*u;
    vec3 p=origin+direction*mix(near,next,jitter);
    float segment=1-exp(-volumeDensity(p)*(next-near));
    if(segment>0)inscatter+=transmittance*segment*volumeLookup(p);
    transmittance*=1-segment;near=next;
  }
  inscatter*=frame.volumeMedium.rgb*frame.volumeParams.z;
}
#ifdef VOLUME_UPDATE_PASS
// What a local light sends to a point of the medium, unshadowed: its radiance
// times its solid angle from there over 4 pi (isotropic phase). Distances
// under half a cell are held there: a cell stands for its volume. `endpoint`:
// the point of the emitter to test visibility to.
vec3 volumeCandidate(LightEmitter light,vec3 p,float size,inout uint state,out vec3 endpoint) {
  endpoint=p;
  vec3 toCentre=light.positionRadius.xyz-p;
  float falloff=localFalloff(dot(toCentre,toCentre));
  if(falloff==0)return vec3(0);
  float floor2=.25*size*size,fraction;
  if(light.radiance.w!=0) {
    vec3 a=light.corners[0].xyz,b=light.corners[1].xyz,c=light.corners[2].xyz;
    vec3 normal=cross(b-a,c-a);float twiceArea=length(normal);
    if(twiceArea<=1e-12)return vec3(0);
    normal/=twiceArea;
    float u=lightRandom(state),v=lightRandom(state);
    endpoint=a+sqrt(u)*((1-v)*(b-a)+v*(c-a));
    vec3 delta=endpoint-p;float distance2=dot(delta,delta);
    if(distance2<=1e-12)return vec3(0);
    float cosine=dot(normal,-delta*inversesqrt(distance2));
    if(light.radiance.w>1)cosine=abs(cosine);
    if(cosine<=0)return vec3(0);
    fraction=.5*twiceArea*cosine/(4*3.14159265359*max(distance2,floor2));
    endpoint=offsetRay(endpoint,dot(normal,-delta)>0?normal:-normal);
  } else {
    float radius=light.positionRadius.w,distance2=max(dot(toCentre,toCentre),floor2);
    float sinMax2=min(1,radius*radius/distance2);
    fraction=.5*sinMax2/(1+sqrt(max(0,1-sinMax2)));
    // A point of the sphere, for a penumbra as wide as the emitter.
    vec3 offset=vec3(lightRandom(state),lightRandom(state),lightRandom(state))*2-1;
    endpoint=light.positionRadius.xyz+offset*(radius*.577);
  }
  vec3 delta=endpoint-p;
  if(dot(delta,delta)<=1e-12)return vec3(0);
  return emitterRadiance(light,-normalize(delta))*(falloff*fraction*frame.localScale.x);
}
// The local lights at a point of the medium, with two visibility rays. Up to
// 64 lights: every one is evaluated. More: 32 candidates drawn from a cell
// pool's slots (regir.glsl), or from the tree by the same normal-free
// weights. Of what they would send unshadowed, the largest term is traced by
// itself and one of the others stands for the rest, chosen by its share.
// (One light drawn by the tree's weights alone, which know neither a spot's
// cone nor which side an emitter faces, left rare bright samples of a far
// light in the cells around a near one: pale fireflies beside a red lamp.)
vec3 volumeLocal(vec3 p,float size,inout uint state) {
  if(!enableLocalLights||frame.localConfig.x==0||frame.lightNodes==0)return vec3(0);
  LightNodes nodes=LightNodes(frame.lightNodes);
  LightEmitters emitters=LightEmitters(frame.lightEmitters);
  uint lights=nodes.data[0].links.w;
  bool every=lights<=64u;
  uint count=every?2u*lights-1u:32u;
  uint pool=every?regirNone:regirPool(p,state);
  vec3 best=vec3(0),bestEndpoint=p,chosen=vec3(0),chosenEndpoint=p,endpoint;
  float bestWeight=0,chosenWeight=0,rest=0;
  for(uint i=0;i<count;++i) {
    uint leaf=i;float pmf=1;
    if(every) {
      if(nodes.data[i].links.w!=1u)continue;
    } else {
      float u=(float(i)+lightRandom(state))/float(count);
      if(pool==regirNone||!regirSample(pool,u,leaf,pmf)) {
        leaf=0;pmf=1;
        while(nodes.data[leaf].links.w>1) {
          uvec4 links=nodes.data[leaf].links;
          float a=regirWeight(nodes.data[links.x],p,p),b=regirWeight(nodes.data[links.y],p,p);
          if(a==0&&b==0){pmf=0;break;}
          float split=lightSplit(a,b);
          if(u<split){leaf=links.x;pmf*=split;u/=split;}else{leaf=links.y;pmf*=1-split;u=(u-split)/(1-split);}
          u=min(u,.99999994);
        }
      }
      if(pmf<=0)continue;
      pmf*=float(count);
    }
    float share;uint emitter=leafEmitter(emitters,nodes.data[leaf].links,state,share);
    vec3 radiance=volumeCandidate(emitters.data[emitter],p,size,state,endpoint)/(pmf*share);
    float weight=dot(radiance,vec3(.2126,.7152,.0722));
    if(weight<=0)continue;
    // The largest so far is held apart; the one it replaces joins the rest.
    if(weight>bestWeight) {
      vec3 r=best,e=bestEndpoint;float w=bestWeight;
      best=radiance;bestEndpoint=endpoint;bestWeight=weight;
      radiance=r;endpoint=e;weight=w;
      if(weight<=0)continue;
    }
    rest+=weight;
    if(lightRandom(state)*rest<weight){chosen=radiance;chosenEndpoint=endpoint;chosenWeight=weight;}
  }
  vec3 sum=vec3(0);
  if(bestWeight>0)sum+=best*localVisibility(p,bestEndpoint);
  if(chosenWeight>0)sum+=chosen*(rest/chosenWeight)*localVisibility(p,chosenEndpoint);
  return sum;
}
void volumeUpdate(uint index) {
  uint n=frame.volumeConfig.y,levels=frame.volumeConfig.z,perLevel=n*n*n,total=perLevel*levels;
  if(frame.volumeGrid==0||index>total)return;
  VolumeCells cells=VolumeCells(frame.volumeGrid);
  uint skyFirst=volumeSkyFirst();
  if(index==total) {
    // The cell past the grids (volumeFar): sixteen directions of the
    // environment a frame, and the sun.
    uint state=hash(frame.volumeFrame.x^0x66617221u);
    vec3 sky=vec3(0);
    for(uint i=0;i<16u;++i) {
      float z=1-2*lightRandom(state),angle=6.28318530718*lightRandom(state),r=sqrt(max(0,1-z*z));
      sky+=environmentRadiance(vec3(r*cos(angle),r*sin(angle),z))*(1.0/16.0);
    }
    if(any(isnan(sky))||any(isinf(sky)))sky=vec3(0);
    vec4 stored=cells.data[skyFirst+total];
    if(frame.volumeFrame.w!=0u)stored=vec4(0);
    float count=min(stored.w+1,32);
    stored=vec4(mix(stored.rgb,sky,1/count),count);
    cells.data[skyFirst+total]=stored;
    cells.data[total]=vec4(stored.rgb+.25*frame.lightRadiance.rgb,count);
    return;
  }
  uint level=index/perLevel,local=index%perLevel;
  ivec3 slot=ivec3(local%n,(local/n)%n,local/(n*n));
  // The cell of this level's window stored in the slot.
  ivec3 origin=volumeOrigin(frame.volumeEye.xyz,level);
  ivec3 cell=origin+volumeWrap(slot-origin);
  float size=volumeCell(level);
  // What the cell holds, and the environment's share of it. The two are
  // averaged apart: the sun and the lamps change and a few samples of them
  // are little noise, while the sky seen through an opening is most often
  // missed and needs many samples (a short average of it made the cells of a
  // covered place flicker like static) but hardly changes.
  vec4 stored=cells.data[index],storedSky=cells.data[skyFirst+index];
  // A cell that was outside last frame's window holds another place's light:
  // it starts from the next level's value there, or empty.
  ivec3 previous=volumeOrigin(frame.volumePrevious.xyz,level);
  bool entered=frame.volumeFrame.w!=0u||any(lessThan(cell,previous))||any(greaterThanEqual(cell,previous+int(n)));
  if(entered) {
    stored=storedSky=vec4(0);
    if(frame.volumeFrame.w==0u) {
      vec3 centre=(vec3(cell)+.5)*size;
      ivec3 outerOrigin=volumeOrigin(frame.volumeEye.xyz,level+1);
      vec4 outer=level+1<levels?volumeFetch(level+1,centre,outerOrigin):vec4(volumeFar().rgb,volumeFar().w>0?1.:0.);
      vec4 outerSky=level+1<levels?volumeFetch(level+1,centre,outerOrigin,skyFirst)
                                  :vec4(cells.data[skyFirst+total].rgb,cells.data[skyFirst+total].w>0?1.:0.);
      if(outer.w>0)stored=vec4(outer.rgb/outer.w,1);
      if(outerSky.w>0)storedSky=vec4(outerSky.rgb/outerSky.w,1);
    }
  }
  bool due=(hash(index)+frame.volumeFrame.x)%max(frame.volumeConfig.w,1u)==0u;
  if(!due) {
    if(entered){cells.data[index]=stored;cells.data[skyFirst+index]=storedSky;}
    return;
  }
  uint state=hash(index^hash(frame.volumeFrame.x^0x766f6c75u));
  vec3 p=(vec3(cell)+vec3(lightRandom(state),lightRandom(state),lightRandom(state)))*size;
  vec3 light=vec3(0),sky=vec3(0);
  // The sun: lightRadiance is its irradiance over pi (see direct()).
  if(any(greaterThan(frame.lightRadiance.rgb,vec3(0)))) {
    vec3 centre=-frame.lightDirection.xyz,direction=centre;
    if(frame.sun.x>0) {
      float angle=6.28318530718*lightRandom(state),u=lightRandom(state);
      vec3 t=normalize(cross(abs(centre.z)<.999?vec3(0,0,1):vec3(0,1,0),centre));
      direction=normalize(centre+frame.sun.x*sqrt(u)*(cos(angle)*t+sin(angle)*cross(centre,t)));
    }
    light+=.25*frame.lightRadiance.rgb*shadow(p,direction);
  }
  // The environment: two uniform directions (their density cancels the phase).
  for(uint i=0;i<2u;++i) {
    float z=1-2*lightRandom(state),angle=6.28318530718*lightRandom(state),r=sqrt(max(0,1-z*z));
    vec3 direction=vec3(r*cos(angle),r*sin(angle),z);
    vec3 radiance=environmentRadiance(direction);
    if(any(greaterThan(radiance,vec3(0))))sky+=.5*radiance*shadow(p,direction);
  }
  light+=volumeLocal(p,size,state);
  if(any(isnan(light))||any(isinf(light)))light=vec3(0);
  if(any(isnan(sky))||any(isinf(sky)))sky=vec3(0);
  float history=float(max(frame.volumeFrame.y,1u));
  float count=min(stored.w+1,history),skyCount=min(storedSky.w+1,history*16);
  vec3 direct=mix(max(stored.rgb-storedSky.rgb,vec3(0)),light,1/count);
  storedSky=vec4(mix(storedSky.rgb,sky,1/skyCount),skyCount);
  // Even so a cell the sky seldom reaches glowed for seconds after a sample
  // that found it, alone among dark ones. Neighbouring cells see nearly the
  // same sky: each update the cell's share is averaged with that of one
  // neighbour its sample point sees (so not through a wall), which spreads
  // every sample over about three cells in each direction.
  {
    uint side=hash(state^0x6e656967u)%6u;
    ivec3 step=ivec3(0);step[side>>1]=(side&1u)!=0u?1:-1;
    ivec3 neighbour=cell+step;
    if(all(greaterThanEqual(neighbour,origin))&&all(lessThan(neighbour,origin+int(n)))) {
      vec4 other=cells.data[skyFirst+volumeIndex(level,neighbour)];
      if(other.w>0&&dot(localVisibility(p,(vec3(neighbour)+.5)*size),vec3(1))>1.5)
        storedSky.rgb=mix(storedSky.rgb,other.rgb,.5);
    }
  }
  cells.data[skyFirst+index]=storedSky;
  cells.data[index]=vec4(direct+storedSky.rgb,count);
}
#endif
