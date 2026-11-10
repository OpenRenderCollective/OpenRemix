// Example scalar-guide adapter for a source whose reference domain is 42.
// Keep storage unchanged while replacing the paired transport convention.
#include "lighting-reference.glsl"
#include "reflection-guide.glsl"
vec4 packReflectionGuide(vec3 normal, float roughness, LightingReference reference) {
  return packReflectionGuide(normal, roughness, lightingReferenceLuminance(reference, 42u));
}
LightingReference reflectionGuideReference(vec4 guide) {
  return LightingReference(guide.w, 42u);
}
