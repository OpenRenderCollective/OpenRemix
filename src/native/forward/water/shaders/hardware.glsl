#include "../../shaders/reflection-sample.glsl"
// Water and opaque reflection rays use the same captured materials, lighting,
// alpha coverage and measured dielectric transport. Only the primary ray differs.
#ifndef LIVE_GI
#define RAY_SCENE_SET 3
#define RAY_FRAGMENT 1
#include "../../bake/shaders/bindings.glsl"
#include "../../bake/shaders/material.glsl"
#include "../../bake/shaders/radiance.glsl"
#endif

vec4 waterHardwareReflection(vec3 position, vec3 normal, vec3 direction, float roughness, vec4 screen) {
  if (screen.a >= .999 || roughness >= waterFrame.reflectionTrace.w) return screen;
  vec3 origin = fma(normal, vec3(rayFrame.trace.y), position);
  uint seed = floatBitsToUint(origin.x) * 73856093u ^ floatBitsToUint(origin.y) * 19349663u ^
              floatBitsToUint(origin.z) * 83492791u ^ 2654435761u;
  Medium medium = initialMedium(origin, seed);
  float distance;
  bool back, miss;
  vec3 value = radiance(origin, direction, medium, seed, distance, back, miss,
#ifdef LIVE_GI
        draw.reflectionDistance, draw.reflectionBounces);
#else
        rayFrame.trace.x, rayFrame.counts.w);
#endif
  // A valid black hit replaces the fallback; sky misses and invalid back faces
  // retain it. Interface Fresnel belongs to the caller, exactly as with SSR.
  if (back || miss || any(isnan(value)) || any(isinf(value))) return screen;
  return vec4(resolveReflection(ReflectionSample(screen.rgb, clamp(screen.a, 0, 1)), max(value, vec3(0))), 1);
}
