#ifndef FORWARD_REFLECTION_MATH_GLSL
#define FORWARD_REFLECTION_MATH_GLSL
// Ray hit tests and probe membership amplify small floating-point changes.
// Use explicit fused operations for cancellation-sensitive calculations, with
// precise results preventing the compiler from splitting or reassociating them.
float reflectionDot(vec3 a, vec3 b) {
  precise float result = fma(a.x, b.x, fma(a.y, b.y, a.z * b.z));
  return result;
}
vec3 reflectionUnit(vec3 value) {
  precise vec3 result = value / sqrt(max(reflectionDot(value, value), 1e-30));
  return result;
}
vec3 reflectionDirection(vec3 incident, vec3 normal) {
  precise vec3 result = fma(normal, vec3(-2 * reflectionDot(normal, incident)), incident);
  return result;
}
vec4 reflectionTransform(mat4 matrix, vec4 point) {
  precise vec4 result = fma(matrix[0], vec4(point.x),
                            fma(matrix[1], vec4(point.y),
                                fma(matrix[2], vec4(point.z), matrix[3] * point.w)));
  return result;
}
#endif
