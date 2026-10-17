#ifndef FORWARD_SCREEN_REFLECTION_GLSL
#define FORWARD_SCREEN_REFLECTION_GLSL
// Caller supplies completed opaque normals, depthPyramid and opaqueColor samplers.
// RGB is scene-linear hit radiance multiplied by confidence in alpha. The caller
// fills missing confidence with probes/sky, then applies its interface Fresnel once.
struct ScreenReflectionView {
  mat4 camera, inverseCamera, view;
  vec4 trace; // distance, hit thickness, normal bias, roughness limit
  uvec4 config; // samples, resolution divisor, step bound, depth levels
  uvec2 extent;
};
float reflectionViewDepth(ScreenReflectionView screen, vec2 uv, float depth) {
  vec4 world = reflectionTransform(screen.inverseCamera, vec4(uv * 2 - 1, depth, 1));
  precise vec3 position = world.xyz / world.w;
  return abs(reflectionTransform(screen.view, vec4(position, 1)).z);
}
float reflectionCellExit(vec2 position, vec2 direction, ivec2 cell, ivec2 dimensions, float cellSize, uvec2 extent) {
  vec2 boundary;
  for (int axis = 0; axis < 2; ++axis) {
    float lower = float(cell[axis]) * cellSize / float(extent[axis]);
    float upper =
        cell[axis] == dimensions[axis] - 1 ? 1 : float(cell[axis] + 1) * cellSize / float(extent[axis]);
    boundary[axis] = direction[axis] > 0 ? upper : lower;
  }
  vec2 distance = vec2(1e30);
  for (int axis = 0; axis < 2; ++axis)
    if (abs(direction[axis]) > 1e-10)
      distance[axis] = max(0, (boundary[axis] - position[axis]) / direction[axis]);
  return min(distance.x, distance.y);
}
vec4 traceScreenReflection(ScreenReflectionView screen, vec3 p, vec3 normal,
                           vec3 direction, vec3 incident, float roughness, vec2 uv) {
  if (screen.config.z == 0u || roughness >= screen.trace.w) return vec4(0);
  float nv = reflectionDot(normal, -incident);
  if (nv <= .01)
    return vec4(0);
  vec3 start = fma(normal, vec3(screen.trace.z), p);
  float distance = screen.trace.x;
  // Stop before crossing the camera plane, where projection becomes singular.
  vec3 viewStart = reflectionTransform(screen.view, vec4(start, 1)).xyz;
  vec3 viewDirection = reflectionTransform(screen.view, vec4(direction, 0)).xyz;
  float viewSign = viewStart.z < 0 ? -1 : 1;
  if (viewDirection.z * viewSign < 0)
    distance = min(distance, (abs(viewStart.z) - max(screen.trace.z, .001)) / abs(viewDirection.z));
  if (distance <= screen.trace.z)
    return vec4(0);
  vec4 a = reflectionTransform(screen.camera, vec4(start, 1));
  vec4 b = reflectionTransform(screen.camera, vec4(fma(direction, vec3(distance), start), 1));
  vec3 origin = a.xyz / a.w, endpoint = b.xyz / b.w;
  origin.xy = fma(origin.xy, vec2(.5), vec2(.5));
  endpoint.xy = fma(endpoint.xy, vec2(.5), vec2(.5));
  vec3 delta = endpoint - origin;
  precise vec2 screenDelta = delta.xy * vec2(screen.extent);
  precise float pixelLength = sqrt(fma(screenDelta.x, screenDelta.x, screenDelta.y * screenDelta.y));
  if (pixelLength < 2)
    return vec4(0);
  float limit = 1;
  for (int axis = 0; axis < 2; ++axis) {
    if (delta[axis] > 1e-10)
      limit = min(limit, (1 - origin[axis]) / delta[axis]);
    if (delta[axis] < -1e-10)
      limit = min(limit, -origin[axis] / delta[axis]);
  }
  // Skip the originating pixel. The normal test below also rejects rays
  // travelling out of a surface that screen-depth quantization overlaps.
  float t = min(limit, 1.5 / pixelLength);
  int mip = 0;
  float epsilon = .01 / pixelLength;
  for (uint step = 0; step < screen.config.z && t < limit; ++step) {
    vec3 q = fma(delta, vec3(t), origin);
    if (any(lessThan(q, vec3(0))) || any(greaterThan(q, vec3(1))))
      break;
    float cellSize = exp2(float(mip));
    ivec2 dimensions = textureSize(depthPyramid, mip);
    ivec2 cell = min(ivec2(q.xy * vec2(screen.extent) / cellSize), dimensions - 1);
    float exit = min(t + reflectionCellExit(q.xy, delta.xy, cell, dimensions, cellSize, screen.extent), limit);
    float endDepth = fma(delta.z, exit, origin.z);
    float surface = texelFetch(depthPyramid, cell, mip).r;
    if (surface >= 1 || max(q.z, endDepth) < surface) {
      t = exit + epsilon;
      mip = min(mip + 1, int(screen.config.w) - 1);
      continue;
    }
    if (mip > 0) {
      --mip;
      continue;
    }
    precise float hitT = q.z >= surface ? t : t + (surface - q.z) / max(delta.z, 1e-10);
    vec3 hit = fma(delta, vec3(hitT), origin);
    float behind = reflectionViewDepth(screen, hit.xy, hit.z) - reflectionViewDepth(screen, hit.xy, surface);
    vec3 hitNormal = texelFetch(normals, cell, 0).xyz;
    if (hitT <= exit + epsilon && behind >= -screen.trace.z && behind <= screen.trace.y &&
        reflectionDot(hitNormal, direction) < -.001) {
      float colorMip = reflectionColorMip(roughness, uv, hit.xy, vec2(screen.extent), nv);
      vec4 color = finiteRadiance(textureLod(opaqueColor, hit.xy, colorMip));
      float edge = min(min(hit.x, hit.y), min(1 - hit.x, 1 - hit.y));
      float confidence = smoothstep(0, .05, edge) * (1 - smoothstep(.8, 1, hitT));
      confidence *= 1 - smoothstep(screen.trace.w * .75, screen.trace.w, roughness);
      confidence *= smoothstep(.01, .1, nv) * color.a;
      return vec4(color.rgb / max(color.a, 1e-5) * confidence, confidence);
    }
    t = exit + epsilon;
    mip = min(1, int(screen.config.w) - 1);
  }
  return vec4(0);
}
#endif
