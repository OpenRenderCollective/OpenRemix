#ifndef OPENREMIX_REFLECTION_GUIDE_REFERENCE
#define OPENREMIX_REFLECTION_GUIDE_REFERENCE
#include "lighting-reference.glsl"
#include "reflection-guide.glsl"
// This scalar guide transports one normalization domain. Keep the convention
// paired here: a different source must replace both encoding and decoding.
// Foreign references become unavailable rather than acquiring a false domain.
vec4 packReflectionGuide(vec3 normal, float roughness, LightingReference reference) {
  return packReflectionGuide(normal, roughness,
      lightingReferenceLuminance(reference, Source2StaticLightingReference));
}
LightingReference reflectionGuideReference(vec4 guide) {
  return LightingReference(guide.w, Source2StaticLightingReference);
}
#endif
