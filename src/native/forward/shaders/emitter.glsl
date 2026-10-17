#ifndef FORWARD_EMITTER_GLSL
#define FORWARD_EMITTER_GLSL
// Matches pathtrace/shaders/light_bvh.glsl: softness is the fraction of the
// outer cone angle occupied by its transition, rather than a cosine interval.
float emitterConeWeight(vec3 axis, float outer, float softness, float focus, vec3 outgoing) {
  float cosine = dot(axis, outgoing);
  float weight = focus > 0 ? pow(max(cosine, 0), focus) : 1;
  if (outer <= -1) return weight;
  if (cosine <= outer) return 0;
  float inner = cos(acos(clamp(outer, -1, 1)) * (1 - clamp(softness, 0, 1)));
  return weight * (inner > outer ? smoothstep(outer, inner, cosine) : 1);
}
// 1-sqrt(1-x) loses distant emitters to float cancellation. The equivalent
// quotient retains their radial inverse-square energy, as in the path tracer.
float sphereOneMinusCos(float distanceSquared, float radius) {
  if (distanceSquared <= radius * radius) return 2;
  float sineSquared = min(1, radius * radius / max(distanceSquared, 1e-20));
  return sineSquared / (1 + sqrt(max(0, 1 - sineSquared)));
}
#endif
