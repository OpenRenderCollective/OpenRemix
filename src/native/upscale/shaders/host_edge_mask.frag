#version 450
// Host pass edge smoothing (runtime/host_passes.cpp), first stage: which
// pixels a run of the host's depth-tested draws changed. 1 where the frame
// differs from its copy taken before the run, else 0.
layout(binding = 0) uniform sampler2D before;
layout(binding = 1) uniform sampler2D after;
layout(location = 0) out vec4 mask;
void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy);
  mask = vec4(any(notEqual(texelFetch(before, p, 0).rgb, texelFetch(after, p, 0).rgb)) ? 1.0 : 0.0);
}
