// A nearest MSAA depth is conservative at silhouette edges, but biases a
// continuous sloped surface toward the camera. At a shoreline that can turn a
// zero-length water connection into a missing layer and an opaque body colour.
// Recover the actual pixel-centre depth only when a cross of sample sets fits
// one plane. Retain nearest depth for discontinuities, sky and unknown patterns.
vec4 waterDepthSamples(ivec2 pixel);
vec2 waterDepthRange(vec4 values) {
  vec2 result = vec2(1, 0);
  for (uint i = 0u; i < target.z; ++i)
    result = vec2(min(result.x, values[i]), max(result.y, values[i]));
  return result;
}
vec2 waterSampleOffset(uint sampleIndex) {
  // Vulkan's standard 2x/4x locations, relative to the pixel centre. The host
  // enables reconstruction only when standardSampleLocations is guaranteed.
  if (target.z == 2u) return sampleIndex == 0u ? vec2(.25) : vec2(-.25);
  const vec2 offsets[4] = vec2[4](vec2(-.125, -.375), vec2(.375, -.125),
                                  vec2(-.375, .125), vec2(.125, .375));
  return offsets[sampleIndex];
}
float waterResolvedDepth(ivec2 pixel) {
  vec4 samples = waterDepthSamples(pixel);
  vec2 range = waterDepthRange(samples);
  const float tolerance = 2.5e-7; // A few D32 rounding units, not a world-space gap allowance.
  if ((target.w & 2u) == 0u || range.y >= 1 || range.y - range.x <= 4 * tolerance ||
      any(lessThan(pixel, ivec2(1))) || any(greaterThanEqual(pixel + 1, ivec2(target.xy))))
    return range.x;
  const ivec2 offsets[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
  vec4 neighbours[4];
  vec4 centres;
  for (uint i = 0u; i < 4u; ++i) {
    neighbours[i] = waterDepthSamples(pixel + offsets[i]);
    vec2 neighbourRange = waterDepthRange(neighbours[i]);
    if (neighbourRange.y >= 1) return range.x;
    centres[i] = (neighbourRange.x + neighbourRange.y) * .5;
  }
  float centre = (range.x + range.y) * .5;
  vec2 gradient = vec2(centres.y - centres.x, centres.w - centres.z) * .5;
  for (uint sampleIndex = 0u; sampleIndex < target.z; ++sampleIndex) {
    vec2 offset = waterSampleOffset(sampleIndex);
    if (abs(samples[sampleIndex] - (centre + dot(gradient, offset))) > tolerance)
      return range.x;
    for (uint i = 0u; i < 4u; ++i)
      if (abs(neighbours[i][sampleIndex] - (centre + dot(gradient, vec2(offsets[i]) + offset))) > tolerance)
        return range.x;
  }
  return centre;
}
