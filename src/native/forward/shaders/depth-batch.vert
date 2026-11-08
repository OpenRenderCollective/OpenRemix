#version 450
#extension GL_GOOGLE_include_directive : require
#include "bindings.glsl"
layout(location = 0) in vec3 position;
layout(location = 1) in uint vertexTag;
layout(location = 0) flat out uint rejectBackfaces;
void main() {
  // Mirrors depth_batch/batch.h. World-space vertices need no material read.
  vec4 world = vec4(position, 1);
  if ((vertexTag & 0x80000000u) != 0u) {
    rejectBackfaces = vertexTag & 16u;
  } else {
    Material m = materials[vertexTag];
    world = m.model * world;
    rejectBackfaces = m.state.w & 16u;
  }
  gl_Position = frame.camera * world;
}
