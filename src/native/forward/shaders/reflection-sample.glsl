#ifndef OPENREMIX_REFLECTION_SAMPLE
#define OPENREMIX_REFLECTION_SAMPLE
// Scene-linear incident radiance, already weighted by source confidence.
// A valid black result has confidence 1; an unavailable result has confidence 0.
// This is independent of the material Fresnel response and pass resource layout.
struct ReflectionSample {
  vec3 radiance;
  float confidence;
};
// Providers own confidence validation/filtering. Do not silently clamp here:
// adapters must preserve the confidence policy of their source.
vec3 resolveReflection(ReflectionSample source, vec3 fallback) {
  return source.radiance + fallback * (1 - source.confidence);
}
#endif
