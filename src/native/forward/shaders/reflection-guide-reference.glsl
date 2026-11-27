#ifndef OPENREMIX_REFLECTION_GUIDE_REFERENCE
#define OPENREMIX_REFLECTION_GUIDE_REFERENCE
#include "lighting-reference.glsl"
#include "reflection-guide.glsl"
// This scalar guide transports one normalization domain. Keep the convention
// paired here: a different source must replace both encoding and decoding.
// Foreign references become unavailable rather than acquiring a false domain.
// The writer's diffuse source fixes the domain at compilation; a reader shared
// by several sources names its own before importing this file.
#ifndef REFLECTION_GUIDE_DOMAIN
#ifdef SPLIT_CASCADES
#define REFLECTION_GUIDE_DOMAIN LiveIncidentLightingReference
#else
#define REFLECTION_GUIDE_DOMAIN Source2StaticLightingReference
#endif
#endif
vec4 packReflectionGuide(vec3 normal, float roughness, LightingReference reference) {
  return packReflectionGuide(normal, roughness,
      lightingReferenceLuminance(reference, REFLECTION_GUIDE_DOMAIN));
}
LightingReference reflectionGuideReference(vec4 guide) {
  return LightingReference(guide.w, REFLECTION_GUIDE_DOMAIN);
}
#endif
