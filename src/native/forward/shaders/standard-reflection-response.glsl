#ifndef OPENREMIX_STANDARD_REFLECTION_RESPONSE
#define OPENREMIX_STANDARD_REFLECTION_RESPONSE
// Standard material response to incident reflection radiance. Source confidence,
// fallback and normalization are resolved before this material-only operation.
vec3 standardReflectionFresnel(vec3 f0, float roughness, float normalView) {
  return f0 + (max(vec3(1 - roughness), f0) - f0) * pow(1 - normalView, 5);
}
#endif
