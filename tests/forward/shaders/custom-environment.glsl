// Example provider with no cubemap/sky atlas access. This is intentionally
// simple; it verifies the query boundary, not a new production lighting model.
vec3 sampleEnvironmentReflection(ReflectionReceiver receiver) {
  vec3 horizon = mix(vec3(.03), vec3(.2,.3,.5), max(receiver.direction.z, 0));
  return mix(horizon, vec3(.1), receiver.roughness * receiver.roughness);
}
