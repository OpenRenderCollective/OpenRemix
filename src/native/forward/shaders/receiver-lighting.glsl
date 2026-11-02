// Diffuse transport returns irradiance / pi, before material response.
// The receiver, rather than the material, determines whether surface charts or
// volume probes supply it. Keep this decision shared by world and viewmodels.
struct ReceiverLighting {
  vec3 diffuse;
  bool aggregate;
};
// Select implementations at compilation; materials never branch on provider IDs.
#ifdef LIVE_GI
#include "diffuse-ray.glsl"
#else
#include "diffuse-baked.glsl"
#endif
