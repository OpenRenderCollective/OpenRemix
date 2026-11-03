// Selected incoming-only source. Material models and direct ownership do not
// depend on whether the provider gathers cascades or traces per receiver.
#ifdef CASCADE_GI
#include "incoming-cascades.glsl"
#else
// Default live incoming source owns its transport dependencies; the material
// pass and receiver-direct storage depend only on this query.
#include "gi.glsl"
vec3 sampleIncomingDiffuse(vec3 position, vec3 normal, vec3 geometric) {
  return liveDiffuse(position, normal, geometric);
}
#endif
