// Select complete receiver contexts with their diffuse implementation. Materials
// consume the same queries without depending on chart or ray representations.
#include "diffuse-values.glsl"
#ifdef LIVE_GI
#include "diffuse-ray.glsl"
#include "receiver-live.glsl"
#elif defined(CASCADE_GI)
#include "diffuse-cascades.glsl"
#include "receiver-live.glsl"
#else
#include "diffuse-baked.glsl"
#include "receiver-baked.glsl"
#endif
