#include "shadow-filter.glsl"
#include "cube.glsl"
layout(set = 0, binding = 7) uniform sampler2DArray staticLocalShadows;
layout(set = 0, binding = 8) uniform sampler2DArray dynamicLocalShadows;
#ifdef VIEWMODEL_PASS
layout(set = 0, binding = 11) uniform sampler2DArray viewModelLocalShadows;
const uint ViewModelLocalShadowFlag = 1u << 0;
bool hasViewModelLocalShadows() { return (frame.clusterInfo.w & ViewModelLocalShadowFlag) != 0u; }
#endif

const uint StaticLocalShadowLayer = 1u;
const uint MovingLocalShadowLayer = 2u;

bool movingLocalShadowRequired(Light light, vec3 position) {
  if (light.corners[0].w <= 0 || light.corners[1].w >= 1) return false;
#ifdef VIEWMODEL_PASS
  // Viewmodel self casters have a separate map and are not in world coverage.
  if (hasViewModelLocalShadows()) return true;
#endif
  if (light.responseWeights.y >= .5) return false;
  if (light.responseWeights.w < .5) return true;
  uint slot = uint(light.corners[0].w) - 1u;
  vec3 ray = position - frame.localPositions[slot].xyz;
  if (dot(ray, ray) < 1e-16) return true;
  vec3 projected = cubeProjection(ray);
  // A 4x4 tent reaches at most two texels from its receiver. Near cube seams,
  // retain the original reprojected filter, which can visit adjacent faces.
  float margin = 4 / frame.localSettings.x + 1e-4;
  if (any(greaterThanEqual(abs(projected.xy), vec2(1 - margin)))) return true;
  vec4 coverage = frame.movingShadowCoverage[slot * 6u + uint(projected.z)];
  return all(greaterThanEqual(projected.xy, coverage.xy - margin)) &&
         all(lessThanEqual(projected.xy, coverage.zw + margin));
}

float localShadowReference(uint face, vec2 pixel, float plane, vec3 normal, float range) {
  // Intersect the actual texel-centre ray with the geometric receiver plane.
  vec2 centre = (pixel + .5) * (2 / frame.localSettings.x) - 1;
  float denominator = dot(normal, normalize(cubeDirection(face, centre)));
  float receiver = abs(denominator) > 1e-7 ? plane / denominator : -1;
  return receiver > 0 ? clamp((receiver - frame.localSettings.y) / range, 0, 1) : 0;
}

vec4 gatherLocalShadowDepth(vec3 coordinate, bool includeStatic, bool includeMoving) {
  // Reorder the gather's footprint into the receiver plane's row order.
  vec4 depth = includeMoving ? textureGather(dynamicLocalShadows, coordinate).wzxy : vec4(1);
#ifdef VIEWMODEL_PASS
  if (includeMoving && hasViewModelLocalShadows())
    depth = min(depth, textureGather(viewModelLocalShadows, coordinate).wzxy);
#endif
  if (includeStatic)
    depth = min(depth, textureGather(staticLocalShadows, coordinate).wzxy);
  return depth;
}

float localShadowVisibility(Light light, vec3 position, vec3 receiverNormal, uint layers, bool allowGather) {
  uint tag = uint(light.corners[0].w);
  float fade = light.corners[1].w;
  if (tag == 0u || layers == 0u || fade >= 1)
    return 1;
  bool includeStatic = (layers & StaticLocalShadowLayer) != 0u;
  bool includeMoving = (layers & MovingLocalShadowLayer) != 0u;
  uint slot = tag - 1u;
  vec4 origin = frame.localPositions[slot];
  vec3 ray = position - origin.xyz;
  float distance = length(ray);
  // The configured shadow range is independent of the light's full influence.
  if (distance >= origin.w || distance < 1e-8)
    return 1;
  float plane = dot(receiverNormal, ray);
  vec3 projected = cubeProjection(ray);
  vec2 receiverPixel = (projected.xy * .5 + .5) * frame.localSettings.x - .5;
  vec2 base = floor(receiverPixel) - 1;
  vec4 weightsX = shadowTentWeights(fract(receiverPixel.x));
  vec4 weightsY = shadowTentWeights(fract(receiverPixel.y));
  float visibility = 0;
  if (allowGather && all(greaterThanEqual(base, vec2(0))) &&
      all(lessThan(base + 3, vec2(frame.localSettings.x)))) {
    uint face = uint(projected.z);
    float layer = float(slot * 6u + face);
    for (int y = 0; y < 4; y += 2)
      for (int x = 0; x < 4; x += 2) {
        vec2 block = base + vec2(x, y);
        vec4 depth = gatherLocalShadowDepth(vec3((block + 1) / frame.localSettings.x, layer), includeStatic, includeMoving);
        vec4 weights = vec4(weightsX[x], weightsX[x + 1], weightsX[x], weightsX[x + 1]) *
                       vec4(weightsY[y], weightsY[y], weightsY[y + 1], weightsY[y + 1]);
        vec4 visible = vec4(1);
        // Cleared texels pass every clamped receiver-plane depth comparison.
        if (!all(equal(depth, vec4(1)))) {
          vec4 reference = vec4(
              localShadowReference(face, block, plane, receiverNormal, origin.w),
              localShadowReference(face, block + vec2(1, 0), plane, receiverNormal, origin.w),
              localShadowReference(face, block + vec2(0, 1), plane, receiverNormal, origin.w),
              localShadowReference(face, block + 1, plane, receiverNormal, origin.w));
          visible = vec4(lessThanEqual(reference, depth));
        }
        visibility += dot(weights, visible);
      }
  } else for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      float weight = weightsX[x] * weightsY[y];
      vec2 tap = (base + vec2(x, y) + .5) * (2 / frame.localSettings.x) - 1;
      // Reproject every tap across cube seams rather than clamp the old face.
      vec3 adjacent = cubeProjection(cubeDirection(uint(projected.z), tap));
      vec2 pixel = clamp(floor((adjacent.xy * .5 + .5) * frame.localSettings.x),
                         vec2(0), vec2(frame.localSettings.x - 1));
      float reference = localShadowReference(uint(adjacent.z), pixel, plane, receiverNormal, origin.w);
      vec3 coordinate = vec3(adjacent.xy * .5 + .5, float(slot * 6u) + adjacent.z);
      float depth = includeMoving ? textureLod(dynamicLocalShadows, coordinate, 0).r : 1;
#ifdef VIEWMODEL_PASS
      if (includeMoving && hasViewModelLocalShadows())
        depth = min(depth, textureLod(viewModelLocalShadows, coordinate, 0).r);
#endif
      if (includeStatic) depth = min(depth, textureLod(staticLocalShadows, coordinate, 0).r);
      visibility += weight * float(reference <= depth);
    }
  // Selection fades shadowing, never the light's radiance or baked visibility.
  return mix(visibility, 1, clamp(fade, 0, 1));
}
