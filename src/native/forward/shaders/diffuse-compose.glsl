#ifndef OPENREMIX_DIFFUSE_COMPOSE
#define OPENREMIX_DIFFUSE_COMPOSE
#include "diffuse-values.glsl"
// The first field contains receiver-direct energy only; incoming contains sky,
// visible emission and bounced transport, both in irradiance / pi. Preserve
// direct attribution so mixed shadows correct only the baked direct portion.
ReceiverLighting composeDirectAndIncoming(ReceiverLighting direct, vec3 incoming) {
  direct.diffuse += incoming;
  // Neither direct-only nor a mixed frozen/live sum is the frozen combined
  // reference used to normalize Source 2 cubemaps.
  direct.reflectionReference = false;
  return direct;
}
#endif
