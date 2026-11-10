// A query-only alternate incoming source; no ray, atlas or receiver-direct API.
vec3 sampleIncomingDiffuse(vec3 position, vec3 normal, vec3 geometric) {
  return mix(vec3(.01), vec3(.3, .4, .5), max(normal.z, 0));
}
