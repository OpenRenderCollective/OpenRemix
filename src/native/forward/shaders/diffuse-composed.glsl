#include "incoming-lighting.glsl"
#include "diffuse-baked-field.glsl"
#include "diffuse-compose.glsl"
// Compile-time source combination. The host must bind ReceiverDirect baked
// data and an incoming-only live source; combined bakes would count energy twice.
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool charted, vec3 chartIrradiance,
                                         bool diffuseRequired, vec3 fallback) {
  ReceiverLighting direct = sampleBakedDiffuseField(position, normal, geometric,
      charted, chartIrradiance, diffuseRequired, vec3(0));
  vec3 incoming = diffuseRequired ? sampleIncomingDiffuse(position, normal, geometric) : vec3(0);
  return composeDirectAndIncoming(direct, incoming);
}
bool needsDiffuseReferenceQuery(bool hasSurfaceField) { return false; }
float diffuseReferenceLuma(vec3 position, vec3 normal) { return -1; }
uint diffuseReflectionReferenceDomain() { return NoLightingReference; }
