#version 450
// A particle's billboard: the emitter's texture or a soft round sprite (a
// streak across velocity-stretched billboards), faded where it nears the
// scene's opaque depth behind it. Output is premultiplied by alpha.
#extension GL_GOOGLE_include_directive : require
#define DRAW 1
#include "common.glsl"
layout(set=1,binding=0) uniform sampler2D sceneDepth; // the frame's opaque depth (captured)
layout(set=1,binding=1) uniform sampler2D sprite;
layout(location=0) in vec4 color;
layout(location=1) in vec2 corner;
layout(location=2) flat in uvec2 shape;
layout(location=3) flat in float softScale;
layout(location=0) out vec4 result;

void main() {
  vec3 rgb = color.rgb;
  float alpha = color.a;
  if (shape.y != 0u) {
    vec4 texel = texture(sprite, corner * .5 + .5);
    rgb *= texel.rgb;
    alpha *= texel.a;
  } else if (shape.x == FACING_VELOCITY) {
    alpha *= (1.0 - corner.x * corner.x) * (1.0 - pow(abs(corner.y), 4.0));
  } else {
    float edge = clamp(1.0 - dot(corner, corner), 0.0, 1.0);
    alpha *= edge * edge;
  }
  if (frame.depthScale.z != 0.0 && softScale > 0.0) {
    float scene = frame.depthScale.y / (texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r - frame.depthScale.x);
    alpha *= clamp((scene - 1.0 / gl_FragCoord.w) * softScale, 0.0, 1.0);
  }
  if (alpha <= 1.0 / 1024.0) discard;
  result = vec4(rgb * alpha, alpha);
}
