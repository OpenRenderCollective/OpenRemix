#ifndef FORWARD_WATER_SCREEN_GLSL
#define FORWARD_WATER_SCREEN_GLSL
// Caller supplies resolved opaque depth (conservative at discontinuities). Read
// completed opaque depth, never the water output currently being written.
float waterOpaqueDepth(ivec2 pixel);
struct WaterScreenHit {
  vec2 uv;
  float distance;
  bool valid;
};
bool waterProject(vec3 point, mat4 projection, ivec2 extent, out vec2 uv, out ivec2 pixel) {
  vec4 clip = projection * vec4(point, 1);
  if (clip.w <= 1e-8) return false;
  vec3 ndc = clip.xyz / clip.w;
  uv = ndc.xy * .5 + .5;
  if (any(lessThan(uv, vec2(0))) || any(greaterThanEqual(uv, vec2(1))) ||
      ndc.z < 0) return false;
  pixel = min(ivec2(uv * vec2(extent)), extent - 1);
  return true;
}
// Returns 0 outside the screen/behind foreground geometry, 1 for sky, and
// 2 with the signed view-depth gap for a usable opaque sample.
uint waterDepthGap(vec3 point, float originDepth, mat4 projection, mat4 inverseProjection,
                    ivec2 extent, float viewSign, float bias, out vec2 uv, out float gap) {
  ivec2 pixel;
  if (!waterProject(point, projection, extent, uv, pixel)) return 0u;
  float depth = waterOpaqueDepth(pixel);
  if (depth >= 1) return 1u;
  vec2 centre = (vec2(pixel) + .5) / vec2(extent);
  vec4 scene = inverseProjection * vec4(centre * 2 - 1, depth, 1);
  if (abs(scene.w) < 1e-8) return 0u;
  float sceneDepth = scene.z / scene.w * viewSign;
  if (sceneDepth < originDepth - bias) return 0u;
  gap = point.z * viewSign - sceneDepth;
  return 2u;
}
WaterScreenHit traceWaterRefraction(vec3 origin, vec3 direction, mat4 projection,
                                    mat4 inverseProjection, ivec2 extent, float viewSign,
                                    float maximumDistance, float bias, float thickness) {
  WaterScreenHit miss = WaterScreenHit(vec2(-1), 0, false);
  if (maximumDistance <= 0 || dot(direction, direction) < 1e-12 ||
      any(lessThanEqual(extent, ivec2(0)))) return miss;
  direction = normalize(direction);
  if (direction.z * viewSign <= 1e-6) return miss;
  float previous = 0, originDepth = origin.z * viewSign;
  for (uint i = 1u; i <= 64u; ++i) {
    float distance = maximumDistance * float(i) / 64;
    vec2 uv;
    float gap;
    uint state = waterDepthGap(origin + direction * distance, originDepth, projection,
                                inverseProjection, extent, viewSign, bias, uv, gap);
    if (state == 0u) return miss;
    if (state == 2u && gap >= 0) {
      float lower = previous, upper = distance;
      for (uint refinement = 0u; refinement < 12u; ++refinement) {
        float middle = (lower + upper) * .5;
        uint sampleState = waterDepthGap(origin + direction * middle, originDepth, projection,
                                          inverseProjection, extent, viewSign, bias, uv, gap);
        if (sampleState == 0u) return miss;
        if (sampleState == 2u && gap >= 0) upper = middle;
        else lower = middle;
      }
      state = waterDepthGap(origin + direction * upper, originDepth, projection,
                             inverseProjection, extent, viewSign, bias, uv, gap);
      // Reject a depth discontinuity rather than grabbing a foreground wall
      // or inventing a long path through a missing screen-space layer.
      if (state == 2u && abs(gap) <= max(thickness, bias))
        return WaterScreenHit(uv, upper, true);
      return miss;
    }
    previous = distance;
  }
  return miss;
}
#endif
