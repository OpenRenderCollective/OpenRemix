#ifndef OPENREMIX_REFLECTION_VALUES
#define OPENREMIX_REFLECTION_VALUES
#include "reflection-sample.glsl"
// Incident environment radiance, before Fresnel or screen/ray confidence.
// The pass supplies its existing probe-position bias and candidate set. Material
// shading does not depend on probe atlas layout or authored/fitted selection.
struct ReflectionReceiver {
  vec3 position;
  vec3 probePosition;
  vec3 normal;
  vec3 direction;
  float roughness;
  float irradianceLuma;
  uint layer;
  uvec2 candidates;
};
#endif
