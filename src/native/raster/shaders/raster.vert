#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec3 position;
layout(location=1) in vec2 texcoord;
layout(location=2) in vec4 color;
layout(location=0) out vec2 uv;
layout(location=1) out vec4 vertexColor;
#ifdef SPRITE
// Sprite overlays (remixapi_InstanceInfoSpriteEXT): the mesh's sprite
// vertices, and what turns a depth-buffer value into a view-space depth.
layout(location=3) in vec3 spriteFrame;    // u1, v1, weight1
layout(location=4) in vec4 spriteSequence; // u2, v2, u3, v3
layout(location=5) in float spriteWeight2;
layout(location=2) out vec3 frame1;
layout(location=3) out vec4 sequence;
layout(location=4) out float weight2;
layout(location=5) flat out vec2 depthScale;
#endif
#ifdef INFECTED
// remixapi_MaterialGradientMapEXT / remixapi_InstanceInfoWoundCutEXT
// (raster/infected.cpp): the bind pose (the mesh's own positions; binding 1),
// the draw's parameters, and the wound's w, h and missing halves (1 where this
// vertex has no q), interpolated as the path tracer's G-buffer does.
#define INFECTED_VERTEX_ONLY 1
#include "infected.glsl"
layout(location=6) in vec3 bindPosition;
layout(location=2) out vec4 woundW;
layout(location=3) out vec3 woundH;
#endif
layout(push_constant) uniform Draw {
  mat4 mvp;
  vec4 tint;
  vec4 uvTransform;
  uvec4 state;
  vec4 factor;
} draw;
void main() {
  gl_Position = draw.mvp * vec4(position, 1.0);
  uv = texcoord * draw.uvTransform.xy + draw.uvTransform.zw;
  vertexColor = color;
#ifdef INFECTED
  woundW = vec4(0);
  woundH = vec3(1, 0, 0);
  if ((floatBitsToUint(infected.wound.w) & 1u) != 0u) {
    float h;
    uint missing;
    woundW = woundVertex(bindPosition, h, missing);
    woundH = vec3(h, float(missing & 1u), float(missing >> 1));
  }
#endif
#ifdef SPRITE
  frame1 = spriteFrame;
  sequence = spriteSequence;
  weight2 = spriteWeight2;
  // A perspective projection's clip z is affine in clip w (the view depth),
  // z = a w + b, whatever the model and view: the depth buffer holds a + b / w.
  vec3 zRow = vec3(draw.mvp[0][2], draw.mvp[1][2], draw.mvp[2][2]);
  vec3 wRow = vec3(draw.mvp[0][3], draw.mvp[1][3], draw.mvp[2][3]);
  float a = dot(zRow, wRow) / max(dot(wRow, wRow), 1e-30);
  depthScale = vec2(a, draw.mvp[3][2] - a * draw.mvp[3][3]);
#endif
}
