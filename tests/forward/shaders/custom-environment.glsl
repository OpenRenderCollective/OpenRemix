// Example provider with no cubemap/sky atlas access. This is intentionally
// simple; it verifies the query boundary, not a new production lighting model.
vec3 sampleEnvironmentReflection(ReflectionReceiver receiver) {
  vec3 horizon = mix(vec3(.03), vec3(.2,.3,.5), max(receiver.direction.z, 0));
  return mix(horizon, vec3(.1), receiver.roughness * receiver.roughness);
}

// Alternative sources need no cubemap candidate list. Called uniformly by the
// world resolve, before lanes outside the dispatch image return.
uvec2 environmentReflectionCandidates(vec3 position, bool valid, uint layer) {
  return uvec2(0);
}

vec3 environmentReflectionPosition(vec3 position, vec3 normal) {
  return position;
}
