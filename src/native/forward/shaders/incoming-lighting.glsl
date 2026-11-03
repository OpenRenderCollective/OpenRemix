// Selected incoming-only source. Material models and direct ownership do not
// depend on whether the provider gathers cascades or traces per receiver.
#ifdef CASCADE_GI
#include "incoming-cascades.glsl"
#else
// Default live incoming source. Its ray transport is prepared by the pass;
// receiver-direct lighting and baked storage do not belong to this query.
vec3 sampleIncomingDiffuse(vec3 position, vec3 normal, vec3 geometric) {
  return liveDiffuse(position, normal, geometric);
}
#endif
