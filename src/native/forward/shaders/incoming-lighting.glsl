// Default live incoming source owns its transport dependencies; the material
// pass and receiver-direct storage depend only on this query.
#include "gi.glsl"
vec3 sampleIncomingDiffuse(vec3 position, vec3 normal, vec3 geometric) {
  return liveDiffuse(position, normal, geometric);
}
