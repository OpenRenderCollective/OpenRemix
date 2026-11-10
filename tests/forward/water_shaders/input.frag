#version 450
layout(push_constant) uniform Draw { float depth; uint mask, leftOnly, width; vec4 color; } draw;
layout(location = 0) out vec4 color;
void main() {
  if (draw.leftOnly != 0u && gl_FragCoord.x >= float(draw.width) * .5) discard;
  gl_SampleMask[0] = int(draw.mask);
  gl_FragDepth = draw.depth;
  color = draw.color;
}
