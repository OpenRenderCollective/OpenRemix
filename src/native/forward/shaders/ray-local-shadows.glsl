#ifndef OPENREMIX_RAY_LOCAL_SHADOWS
#define OPENREMIX_RAY_LOCAL_SHADOWS
// Ray-traced visibility against the live ray scene, for the receivers that
// carry it.
//
// Local lights without a shadow map: the map budget covers the lights nearest
// the camera; every other light used to shade the scene unshadowed, through
// walls. Mapped lights keep their maps; a light fading in or out of a map slot
// blends the two.
//
// The sun: its cascades cover a bounded distance at a bounded resolution and
// leak at thin walls and beyond their range. One ray replaces them.
#ifndef RAY_SCENE_SET
#define RAY_SCENE_SET 3
#endif
#include "ray-transport.glsl"
#include "local-light-record.glsl"
// rayFrame.counts.w - 1 selects what is traced; the options behind it can
// change without replacing the receivers.
const uint RayLocalShadowMode = 1u, RaySunShadowMode = 2u;
bool rayShadowMode(uint mode) { return ((rayFrame.counts.w - 1u) & mode) != 0u; }
// Rays left for this fragment; rayFrame.counts.z is the configured budget.
// Lights beyond it stay unshadowed, as every unmapped light was before.
uint rayLocalShadowBudget = 0xffffffffu;
// `mapped` is the shadow-map visibility, which is one for a light without a
// slot. `direction` and `distance` point from the receiver to the light.
float rayLocalShadow(Light light, vec3 position, vec3 receiverNormal, vec3 direction, float distance,
                     float mapped) {
  // corners[0].w: shadow slot + 1; corners[1].w: unshadowed blend of that slot.
  float unmapped = light.corners[0].w > 0 ? clamp(light.corners[1].w, 0, 1) : 1;
  if (unmapped <= 0 || !rayShadowMode(RayLocalShadowMode)) return mapped;
  // The receiver's own surface hides a light below its geometric horizon.
  if (dot(receiverNormal, direction) <= 0) return mapped * (1 - unmapped);
  if (rayLocalShadowBudget == 0xffffffffu) rayLocalShadowBudget = rayFrame.counts.z;
  if (rayLocalShadowBudget == 0u) return mapped;
  float reach = distance - max(light.positionRadius.w, rayFrame.trace.y);
  if (reach <= rayFrame.trace.y * 2) return mapped;
  --rayLocalShadowBudget;
  uint seed = floatBitsToUint(position.x) * 73856093u ^ floatBitsToUint(position.y) * 19349663u ^
              floatBitsToUint(position.z) * 83492791u;
  vec3 passing = visibility(fma(receiverNormal, vec3(rayFrame.trace.y), position), direction, reach, seed,
                            airMedium());
  return mapped * mix(1, dot(passing, vec3(1.0 / 3.0)), unmapped);
}
// `mapped` is the cascades' visibility; `direction` points at the sun. The
// ray looks for casters within the sun shadow distance (rayFrame.trace.x) and
// takes the place of the cascades: a product would keep their leaks' opposite,
// false shadowing at their edges.
float raySunShadow(vec3 position, vec3 receiverNormal, vec3 direction, float mapped) {
  if (!rayShadowMode(RaySunShadowMode)) return mapped;
  if (dot(receiverNormal, direction) <= 0) return 0;
  uint seed = floatBitsToUint(position.x) * 19349663u ^ floatBitsToUint(position.y) * 83492791u ^
              floatBitsToUint(position.z) * 73856093u;
  vec3 passing = visibility(fma(receiverNormal, vec3(rayFrame.trace.y), position), direction,
                            rayFrame.trace.x, seed, airMedium());
  return dot(passing, vec3(1.0 / 3.0));
}
#endif
