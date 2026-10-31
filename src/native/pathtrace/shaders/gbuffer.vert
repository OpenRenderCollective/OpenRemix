#version 460
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
#include "debug.glsl"
layout(push_constant) uniform Draw {
  mat4 model;
  vec2 jitter;
  uint surfaceIndex;
  uint stableDepth;
  vec4 reverseDepth;
} draw;
layout(location=0) out vec3 position;
layout(location=1) flat out vec3 normal;
layout(location=2) out vec2 uv;
layout(location=3) out vec4 color;
layout(location=4) out vec3 previousPosition;
layout(location=5) flat out float triangleIdentity;
layout(location=6) flat out vec3 tangent;
layout(location=7) flat out vec3 bitangent;
layout(location=8) out vec3 authoredNormal;
// The triangle's world vertices and vertex normals (terminatorLift).
layout(location=9) flat out vec3 corner0;
layout(location=10) flat out vec3 corner1;
layout(location=11) flat out vec3 corner2;
layout(location=12) flat out vec3 cornerNormal0;
layout(location=13) flat out vec3 cornerNormal1;
layout(location=14) flat out vec3 cornerNormal2;
void main() {
  Surface s=surfaces[draw.surfaceIndex];
  uint index=s.indices.i[gl_VertexIndex];
  Vertex v=s.vertices.v[index];
  position=(draw.model*vec4(v.position.xyz,1)).xyz;
  uint triangle=uint(gl_VertexIndex)/3*3;
  triangleIdentity=frame.splitLocal.w==3?debugTriangle(s.debugIdentity,triangle/3):0;
  vec3 a=s.vertices.v[s.indices.i[triangle]].position.xyz;
  vec3 b=s.vertices.v[s.indices.i[triangle+1]].position.xyz;
  vec3 c=s.vertices.v[s.indices.i[triangle+2]].position.xyz;
  // Match ray-hit geometric normals, including mirrored and skinned triangles.
  normal=cross(mat3(draw.model)*(b-a),mat3(draw.model)*(c-a));
  mat3 normalMatrix=transpose(inverse(mat3(draw.model)));
  authoredNormal=normalMatrix*vertexNormal(v);
  corner0=(draw.model*vec4(a,1)).xyz;corner1=(draw.model*vec4(b,1)).xyz;corner2=(draw.model*vec4(c,1)).xyz;
  cornerNormal0=normalize(normalMatrix*vertexNormal(s.vertices.v[s.indices.i[triangle]]));
  cornerNormal1=normalize(normalMatrix*vertexNormal(s.vertices.v[s.indices.i[triangle+1]]));
  cornerNormal2=normalize(normalMatrix*vertexNormal(s.vertices.v[s.indices.i[triangle+2]]));
  vec2 ua=s.vertices.v[s.indices.i[triangle]].uv.xy;
  vec2 ub=s.vertices.v[s.indices.i[triangle+1]].uv.xy;
  vec2 uc=s.vertices.v[s.indices.i[triangle+2]].uv.xy;
  mat3 basis=materialBasis(normalize(normal),mat3(draw.model)*(b-a),mat3(draw.model)*(c-a),ub-ua,uc-ua);
  tangent=basis[0];bitangent=basis[1];
  uv=v.uv.xy;color=v.color;previousPosition=position;
  if(s.previousVertices!=0) {
    Vertices previous=Vertices(s.previousVertices);
    previousPosition=(s.previousModel*vec4(previous.v[index].position.xyz,1)).xyz;
  }
  gl_Position=frame.camera*vec4(position,1);
  gl_Position.z=dot(draw.reverseDepth,vec4(position,1));
  // A view model (stableDepth bit 1) is drawn on a cleared depth buffer after
  // the scene: ordered by its own depth only (reverse, 1e-3/w: nearer is
  // larger), and seen nearer than the world camera's near plane.
  if((draw.stableDepth&2u)!=0u)gl_Position.z=1e-3;
  gl_Position.xy-=2*draw.jitter/vec2(frame.target.xy)*gl_Position.w;
}
