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
// Clip-space projection and the original view Z stay independent so the
// depth comparison uses the same view-space convention as the opaque layer.
struct WaterRefractionRay {
  vec4 clipOrigin, clipDirection;
  float originZ, directionZ;
};
const uint WaterDepthRejected = 0u, WaterDepthSky = 1u, WaterDepthOpaque = 2u;
bool waterProject(vec4 clip, ivec2 extent, out vec2 uv, out ivec2 pixel) {
  if (clip.w <= 1e-8) return false;
  vec3 ndc = clip.xyz / clip.w;
  uv = ndc.xy * .5 + .5;
  if (any(lessThan(uv, vec2(0))) || any(greaterThanEqual(uv, vec2(1))) ||
      ndc.z < 0) return false;
  pixel = min(ivec2(uv * vec2(extent)), extent - 1);
  return true;
}
// Only an opaque sample supplies a signed depth gap.
uint waterDepthGap(WaterRefractionRay ray, float distance, float originDepth, mat4 inverseProjection,
                   ivec2 extent, float viewSign, float bias, bool depthOnly, out vec2 uv, out float gap) {
  vec4 clip = ray.clipOrigin + ray.clipDirection * distance;
  ivec2 pixel;
  if (!waterProject(clip, extent, uv, pixel)) return WaterDepthRejected;
  float depth = waterOpaqueDepth(pixel);
  if (depth >= 1) return WaterDepthSky;
  vec2 scene;
  if (depthOnly) {
    scene = inverseProjection[2].zw * depth + inverseProjection[3].zw;
  } else {
    vec2 centre = (vec2(pixel) + .5) / vec2(extent);
    scene = (inverseProjection * vec4(centre * 2 - 1, depth, 1)).zw;
  }
  if (abs(scene.y) < 1e-8) return WaterDepthRejected;
  float sceneDepth = scene.x / scene.y * viewSign;
  if (sceneDepth < originDepth - bias) return WaterDepthRejected;
  gap = (ray.originZ + ray.directionZ * distance) * viewSign - sceneDepth;
  return WaterDepthOpaque;
}
WaterScreenHit traceWaterRefraction(vec3 origin, vec3 direction, mat4 projection,
                                    mat4 inverseProjection, ivec2 extent, float viewSign,
                                    float maximumDistance, float bias, float thickness) {
  WaterScreenHit miss = WaterScreenHit(vec2(-1), 0, false);
  if (maximumDistance <= 0 || dot(direction, direction) < 1e-12 ||
      any(lessThanEqual(extent, ivec2(0)))) return miss;
  direction = normalize(direction);
  if (direction.z * viewSign <= 1e-6) return miss;
  // Standard perspective and orthographic projections reconstruct view Z/W
  // from depth alone. Keep the full path for oblique or custom projections.
  bool depthOnly = all(equal(inverseProjection[0].zw, vec2(0))) &&
                   all(equal(inverseProjection[1].zw, vec2(0)));
  WaterRefractionRay ray = WaterRefractionRay(projection * vec4(origin, 1),
      projection * vec4(direction, 0), origin.z, direction.z);
  const uint MarchSteps = 64u;
  float tolerance = max(thickness, bias);
  // Eight bisections shrink the coarse interval by 256. Keep the
  // original precision when that interval would exceed 2.5% of tolerance.
  uint refinements = maximumDistance / float(MarchSteps * 256u) <= .025 * tolerance ? 8u : 12u;
  float previous = 0, originDepth = origin.z * viewSign;
  for (uint i = 1u; i <= MarchSteps; ++i) {
    float distance = maximumDistance * float(i) / float(MarchSteps);
    vec2 uv;
    float gap;
    uint state = waterDepthGap(ray, distance, originDepth,
                                inverseProjection, extent, viewSign, bias, depthOnly, uv, gap);
    if (state == WaterDepthRejected) return miss;
    if (state == WaterDepthOpaque && gap >= 0) {
      float lower = previous, upper = distance;
      // Keep the last opaque upper sample along with its distance.
      vec2 upperUv = uv;
      float upperGap = gap;
      for (uint refinement = 0u; refinement < refinements; ++refinement) {
        float middle = (lower + upper) * .5;
        uint sampleState = waterDepthGap(ray, middle, originDepth,
                                          inverseProjection, extent, viewSign, bias, depthOnly, uv, gap);
        if (sampleState == WaterDepthRejected) return miss;
        if (sampleState == WaterDepthOpaque && gap >= 0) {
          upper = middle;
          upperUv = uv;
          upperGap = gap;
        } else {
          lower = middle;
        }
      }
      // Reject a depth discontinuity rather than grabbing a foreground wall
      // or inventing a long path through a missing screen-space layer.
      if (abs(upperGap) <= tolerance)
        return WaterScreenHit(upperUv, upper, true);
      return miss;
    }
    previous = distance;
  }
  return miss;
}
#endif
