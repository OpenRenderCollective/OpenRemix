// Alternate source with a deliberately different context: no chart coordinate,
// probe stencil, atlas values or packed receiver storage is read here.
#include "diffuse-values.glsl"
struct ReceiverLightContext { vec3 direction; };
ReceiverLightContext prepareReceiverLighting(ShadingReceiver receiver, vec3 normal) {
  return ReceiverLightContext(normal);
}
ReceiverLightContext prepareVolumeReceiverLighting(ShadingReceiver receiver, vec3 normal) {
  return ReceiverLightContext(normal);
}
ReceiverLighting sampleDiffuseLighting(ShadingReceiver receiver, vec3 normal,
                                      ReceiverLightContext context, bool diffuseRequired) {
  vec3 incident = mix(vec3(.03), vec3(.2,.3,.5), max(context.direction.z, 0));
  return ReceiverLighting(diffuseRequired ? incident : vec3(0), DirectLightingPolicy(false, true, false), true);
}
bool receiverNeedsDiffuseReference(ReceiverLightContext context) { return false; }
float diffuseReferenceLuma(vec3 position, vec3 normal) { return -1; }
float receiverSunVisibility(ReceiverLightContext context) { return 1; }
vec3 receiverBakedCoverage(ShadingReceiver receiver, vec3 normal, ReceiverLightContext context) {
  return vec3(0); // no compatible baked reference field
}
vec3 receiverBakedDiffuse(ShadingReceiver receiver, vec3 normal, ReceiverLightContext context, vec3 fallback) {
  return fallback;
}

uint diffuseReflectionReferenceDomain() { return 42u; }
