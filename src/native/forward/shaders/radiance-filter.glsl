// Footprint selection adapted from Facepunch's s&box SSR shader.
// Copyright (c) 2025 Facepunch Studios Ltd. See SBOX-LICENSE.txt.
float reflectionColorMip(float roughness, vec2 surfaceUv, vec2 hitUv, vec2 dimensions, float nv) {
  float distancePixels = length((hitUv - surfaceUv) * dimensions);
  // Stretch the lobe at grazing angles; use the geometric mean of its axes
  // for one isotropic trilinear lookup. Near mirrors stay on the base mip.
  float footprint = roughness * roughness * distancePixels * inversesqrt(max(nv, .01));
  return log2(max(footprint, 1));
}

vec4 finiteRadiance(vec4 value) {
  value = mix(value, vec4(0), isnan(value));
  return clamp(value, vec4(0), vec4(65504, 65504, 65504, 1));
}

// Area-weighted reduction keeps the colour pyramid aligned with normalized UVs
// even when a level has an odd width/height or an axis has already reached one.
// RGB and coverage stay premultiplied until the trace consumes the filtered hit.
vec4 reduceRadiance(sampler2D source, ivec2 pixel, ivec2 outputSize) {
  vec2 scale = vec2(textureSize(source, 0)) / vec2(outputSize);
  vec2 lower = vec2(pixel) * scale;
  vec2 upper = min(vec2(pixel + 1) * scale, vec2(textureSize(source, 0)));
  ivec2 begin = ivec2(floor(lower)), end = ivec2(ceil(upper));
  vec4 total = vec4(0);
  for (int y = begin.y; y < end.y; ++y)
    for (int x = begin.x; x < end.x; ++x) {
      vec2 coverage = max(vec2(0), min(upper, vec2(x + 1, y + 1)) - max(lower, vec2(x, y)));
      total += texelFetch(source, ivec2(x, y), 0) * (coverage.x * coverage.y);
    }
  return finiteRadiance(total / (scale.x * scale.y));
}
