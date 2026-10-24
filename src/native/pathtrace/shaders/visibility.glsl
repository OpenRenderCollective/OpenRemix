// Shadow queries carry distance and transmission, without diffuse shading,
// motion vertices or cache metadata. SIGMA needs the nearest blocker; ordinary
// visibility can terminate at any accepted opaque blocker.
struct VisibilityHit { float t; bool blocked; vec3 p; vec3 n; vec3 transmission; };
void queryVisibility(accelerationStructureEXT scene,vec3 origin,vec3 direction,inout VisibilityHit hit,bool anyOpaque) {
  rayQueryEXT query;
  profileRay(8);
  rayQueryInitializeEXT(query,scene,traceFlags(),shadowMask(),origin,0,direction,hit.t);
  while(rayQueryProceedEXT(query)) {
    if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionTriangleEXT)continue;
    profileRay(9);
    Surface s=surfaces[rayQueryGetIntersectionInstanceCustomIndexEXT(query,false)+rayQueryGetIntersectionGeometryIndexEXT(query,false)];
    s.indices=s.traceIndices;
    if(enableDecals&&decalSurface(s))continue; // Paint does not cast a separate sheet shadow.
    if((s.state.w&4)==0&&!rayQueryGetIntersectionFrontFaceEXT(query,false)){profileRay(12);continue;}
    profileAlpha(s,8);
    if(!acceptCandidate(s,rayQueryGetIntersectionPrimitiveIndexEXT(query,false),rayQueryGetIntersectionBarycentricsEXT(query,false))){profileRay(11);continue;}
    if(anyOpaque&&(s.state.w&2)==0) {hit.blocked=true;rayQueryTerminateEXT(query);return;}
    rayQueryConfirmIntersectionEXT(query);
  }
  if(rayQueryGetIntersectionTypeEXT(query,true)==gl_RayQueryCommittedIntersectionNoneEXT)return;
  profileRay(13);
  hit.t=rayQueryGetIntersectionTEXT(query,true);
  Surface s=surfaces[rayQueryGetIntersectionInstanceCustomIndexEXT(query,true)+rayQueryGetIntersectionGeometryIndexEXT(query,true)];
  s.indices=s.traceIndices;
  hit.blocked=(s.state.w&2)==0;
  if(hit.blocked)return;
  uint tri=rayQueryGetIntersectionPrimitiveIndexEXT(query,true)*3;
  vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,true);
  vec4 color=albedo(s,tri/3,bary);
  if(!blendedAtAlpha(s,color.a)){hit.blocked=true;return;}
  vec3 a=s.vertices.v[s.indices.i[tri]].position.xyz,b=s.vertices.v[s.indices.i[tri+1]].position.xyz,c=s.vertices.v[s.indices.i[tri+2]].position.xyz;
  mat4x3 model=rayQueryGetIntersectionObjectToWorldEXT(query,true);
  hit.p=model*vec4(a+bary.x*(b-a)+bary.y*(c-a),1);
  hit.n=normalize(cross(model*vec4(b-a,0),model*vec4(c-a,0)));
  if(dot(hit.n,direction)>0)hit.n=-hit.n;
  hit.transmission=sheetTransmission(s,color,tri/3,bary,abs(dot(hit.n,direction)),hit.t);
}
VisibilityHit visibilityHit(vec3 origin,vec3 direction,bool anyOpaque) {
  VisibilityHit hit;hit.t=1e30;hit.blocked=false;
  if((frame.target.z&1)!=0)queryVisibility(staticScene,origin,direction,hit,anyOpaque);
  if(anyOpaque&&hit.blocked)return hit;
  if((frame.target.z&2)!=0)queryVisibility(dynamicScene,origin,direction,hit,anyOpaque);
  return hit;
}
// rtx.native.pathtrace.shadowEarlyOut (specialization constant
// shadowEarlyOut). The layered transmission loops (shadow, localVisibility,
// sunVisibility without SIGMA) return zero once any blocker exists anywhere
// on the ray, so a layer may stop at the first blocker it meets: an accepted
// opaque candidate terminates, and hardware-opaque geometry commits under
// gl_RayFlagsTerminateOnFirstHitEXT. Transmitting sheets are never
// confirmed (a confirmed sheet would end first-hit traversal); the nearest
// accepted one is tracked here instead and shaded after traversal exactly as
// the closest-hit query shades its committed sheet. Duplicate candidate
// reports cannot change a nearest-sheet choice. profile: the ray profile
// base (8 visibility, 16 local occluder) with the closest-hit layout.
struct NearestSheet { float t; uint surface; uint primitive; vec2 bary; vec3 p; vec3 n; };
bool firstBlocker(accelerationStructureEXT scene,vec3 origin,vec3 direction,float maximum,inout NearestSheet sheet,uint profile) {
  rayQueryEXT query;
  profileRay(profile);
  rayQueryInitializeEXT(query,scene,traceFlags()|gl_RayFlagsTerminateOnFirstHitEXT,shadowMask(),origin,0,direction,maximum);
  while(rayQueryProceedEXT(query)) {
    if(rayQueryGetIntersectionTypeEXT(query,false)!=gl_RayQueryCandidateIntersectionTriangleEXT)continue;
    profileRay(profile+1);
    uint index=rayQueryGetIntersectionInstanceCustomIndexEXT(query,false)+rayQueryGetIntersectionGeometryIndexEXT(query,false);
    Surface s=surfaces[index];
    s.indices=s.traceIndices;
    if(enableDecals&&decalSurface(s))continue; // Paint does not cast a separate sheet shadow.
    if((s.state.w&4)==0&&!rayQueryGetIntersectionFrontFaceEXT(query,false)){profileRay(profile+4);continue;}
    profileAlpha(s,profile);
    uint primitive=rayQueryGetIntersectionPrimitiveIndexEXT(query,false);
    vec2 bary=rayQueryGetIntersectionBarycentricsEXT(query,false);
    if(!acceptCandidate(s,primitive,bary)){profileRay(profile+3);continue;}
    if((s.state.w&2)==0) {rayQueryTerminateEXT(query);return true;}
    // A later candidate at an equal distance replaces the sheet, as a later
    // confirmation replaces an equally distant committed one.
    float t=rayQueryGetIntersectionTEXT(query,false);
    if(t>sheet.t)continue;
    uint tri=primitive*3;
    vec3 a=s.vertices.v[s.indices.i[tri]].position.xyz,b=s.vertices.v[s.indices.i[tri+1]].position.xyz,c=s.vertices.v[s.indices.i[tri+2]].position.xyz;
    mat4x3 model=rayQueryGetIntersectionObjectToWorldEXT(query,false);
    sheet.t=t;sheet.surface=index;sheet.primitive=primitive;sheet.bary=bary;
    sheet.p=model*vec4(a+bary.x*(b-a)+bary.y*(c-a),1);
    sheet.n=normalize(cross(model*vec4(b-a,0),model*vec4(c-a,0)));
  }
  // Only hardware-opaque geometry commits: sheets are never confirmed.
  return rayQueryGetIntersectionTypeEXT(query,true)!=gl_RayQueryCommittedIntersectionNoneEXT;
}
// One layer: blocked, the nearest transmitting sheet, or a miss (t==maximum),
// as visibilityHit(anyOpaque) and localOccluder report it. The dynamic query
// stops at the static sheet, as the closest-hit layers did.
VisibilityHit earlyVisibilityHit(vec3 origin,vec3 direction,float maximum,uint profile) {
  VisibilityHit hit;hit.t=maximum;hit.blocked=false;
  NearestSheet sheet;sheet.t=maximum;sheet.surface=~0u;
  if((frame.target.z&1)!=0&&firstBlocker(staticScene,origin,direction,maximum,sheet,profile)){hit.blocked=true;return hit;}
  if((frame.target.z&2)!=0&&firstBlocker(dynamicScene,origin,direction,sheet.t,sheet,profile)){hit.blocked=true;return hit;}
  if(sheet.surface==~0u)return hit;
  profileRay(profile+5);
  hit.t=sheet.t;
  Surface s=surfaces[sheet.surface];
  s.indices=s.traceIndices;
  vec4 color=albedo(s,sheet.primitive,sheet.bary);
  if(!blendedAtAlpha(s,color.a)){hit.blocked=true;return hit;}
  hit.p=sheet.p;hit.n=sheet.n;
  if(dot(hit.n,direction)>0)hit.n=-hit.n;
  hit.transmission=sheetTransmission(s,color,sheet.primitive,sheet.bary,abs(dot(hit.n,direction)),hit.t);
  return hit;
}
