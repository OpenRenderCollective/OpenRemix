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
// LIT (state.w bit 14): the mean radiance incident at the vertex, from the
// path tracer's volumetric cells (pathtrace/shaders/volume.glsl: nested grids
// around `eye`, each level's cells twice the size of the one before, a cell
// addressed by its world coordinate modulo the grid's size; w: its samples).
// x below zero: not lit.
layout(location=6) out vec3 spriteLight;
layout(set=2,binding=0) uniform SpriteLighting {
  mat4 worldFromClip;
  vec4 eye;     // w: a cell of the first level, in world units
  uvec4 config; // lit, cells a side, levels, display bits
  vec4 display;
} lighting;
layout(std430,set=2,binding=1) readonly buffer SpriteCells { vec4 data[]; } cells;
float cellSize(uint level) { return lighting.eye.w * float(1u << level); }
ivec3 cellOrigin(uint level) { return ivec3(floor(lighting.eye.xyz / cellSize(level))) - int(lighting.config.y >> 1); }
uint cellIndex(uint level, ivec3 cell) {
  uint n = lighting.config.y;
  float size = float(n);
  uvec3 m = uvec3(cell - ivec3(floor(vec3(cell) / size) * size));
  return ((level * n + m.z) * n + m.y) * n + m.x;
}
vec4 cellFetch(uint level, vec3 p, ivec3 origin) {
  int n = int(lighting.config.y);
  vec3 q = clamp(p / cellSize(level) - .5, vec3(origin), vec3(origin + n - 1));
  ivec3 base = min(ivec3(floor(q)), origin + n - 2);
  vec3 f = q - vec3(base);
  vec4 sum = vec4(0);
  for (int corner = 0; corner < 8; ++corner) {
    ivec3 offset = ivec3(corner & 1, (corner >> 1) & 1, corner >> 2);
    vec3 w3 = mix(1 - f, f, vec3(offset));
    vec4 cell = cells.data[cellIndex(level, base + offset)];
    if (cell.w > 0) sum += vec4(cell.rgb, 1) * (w3.x * w3.y * w3.z);
  }
  return sum;
}
vec3 cellFar() {
  uint n = lighting.config.y;
  return cells.data[n * n * n * lighting.config.z].rgb;
}
vec3 cellLookup(vec3 p) {
  uint levels = lighting.config.z;
  int n = int(lighting.config.y);
  for (uint level = 0; level < levels; ++level) {
    ivec3 origin = cellOrigin(level);
    vec3 q = p / cellSize(level) - .5;
    vec3 inside = min(q - vec3(origin), vec3(origin + n - 1) - q);
    float border = min(inside.x, min(inside.y, inside.z));
    bool last = level + 1 == levels;
    if (border < 0 && !last) continue;
    if (border <= 0) break;
    vec4 value = cellFetch(level, p, origin);
    vec3 result = value.w > 0 ? value.rgb / value.w : vec3(0);
    float weight = clamp(border * .5, 0, 1) * (value.w > 0 ? 1. : 0.);
    if (weight < 1) {
      vec4 outer = last ? vec4(cellFar(), 1) : cellFetch(level + 1, p, cellOrigin(level + 1));
      if (outer.w > 0) result = mix(outer.rgb / outer.w, result, weight);
    }
    return result;
  }
  return cellFar();
}
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
  spriteLight = vec3(-1);
  if (((draw.state.w >> 14) & 1u) != 0u && lighting.config.x != 0u) {
    vec4 world = lighting.worldFromClip * gl_Position;
    if (abs(world.w) > 1e-20) spriteLight = max(cellLookup(world.xyz / world.w), vec3(0));
  }
#endif
}
