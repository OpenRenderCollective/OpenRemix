const uint ProbeDepthSide = 8u;
vec3 probeDepthDirection(uint texel, uint sampleIndex) {
  vec2 uv = (vec2(texel % ProbeDepthSide, texel / ProbeDepthSide) +
             vec2(.25) + .5 * vec2(sampleIndex & 1u, sampleIndex >> 1u)) / float(ProbeDepthSide);
  vec2 f = uv * 2 - 1;
  vec3 n = vec3(f, 1 - abs(f.x) - abs(f.y));
  if (n.z < 0) n.xy = (1 - abs(n.yx)) * mix(vec2(-1), vec2(1), greaterThanEqual(n.xy, vec2(0)));
  return normalize(n);
}
vec2 probeDepthUv(vec3 n) {
  n /= abs(n.x) + abs(n.y) + abs(n.z);
  if (n.z < 0) n.xy = (1 - abs(n.yx)) * mix(vec2(-1), vec2(1), greaterThanEqual(n.xy, vec2(0)));
  return n.xy * .5 + .5;
}
ivec2 probeDepthFold(ivec2 p) {
  const int side = int(ProbeDepthSide);
  if (p.x < 0 || p.x >= side) {
    p.x = p.x < 0 ? -1 - p.x : 2 * side - 1 - p.x;
    p.y = side - 1 - p.y;
  }
  if (p.y < 0 || p.y >= side) {
    p.y = p.y < 0 ? -1 - p.y : 2 * side - 1 - p.y;
    p.x = side - 1 - p.x;
  }
  return p;
}
