// Default live incoming source. Its ray transport is prepared by the pass;
// receiver-direct lighting and baked storage do not belong to this query.
vec3 sampleIncomingDiffuse(vec3 position, vec3 normal, vec3 geometric) {
  return liveDiffuse(position, normal, geometric);
}
