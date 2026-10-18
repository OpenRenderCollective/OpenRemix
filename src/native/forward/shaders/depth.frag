#version 460
#extension GL_GOOGLE_include_directive : require
#include "bindings.glsl"
#include "material.glsl"
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 vertexColor;
layout(push_constant) uniform Draw { uint index; uint cascade; } draw;
void main() {
  Material m = materials[draw.index];
  // Match visible world faces and cutout holes before writing any depth.
  if ((m.state.w & 16u) != 0u && !gl_FrontFacing) discard;
  alphaTest(m, surfaceColor(m, uv, vertexColor).a);
}
