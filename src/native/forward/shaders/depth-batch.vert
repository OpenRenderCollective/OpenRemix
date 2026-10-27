#version 450
#extension GL_GOOGLE_include_directive : require
#include "bindings.glsl"
layout(location = 0) in vec3 position;
layout(location = 1) in uint materialIndex;
layout(location = 0) flat out uint rejectBackfaces;
void main() {
  Material m = materials[materialIndex];
  vec4 world = m.model * vec4(position, 1);
  gl_Position = frame.camera * world;
  rejectBackfaces = m.state.w & 16u;
}
