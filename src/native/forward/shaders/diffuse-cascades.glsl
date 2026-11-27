// Split Radiance Cascades diffuse implementation. Selected at shader compilation.
// The query reads probes; it traces nothing, so this source needs no ray queries.
#include "../ray/cascade_shaders/query.glsl"
ReceiverLighting evaluateReceiverLighting(vec3 position, vec3 normal, vec3 geometric,
                                         bool diffuseRequired, vec3 fallback) {
  ReceiverLighting lighting = ReceiverLighting(vec3(0), analyticDirectLighting(false), false);
  // Evaluated even where no diffuse lobe needs it (metals, black albedo): the
  // incident lighting also bounds the sky a reflection fallback may add, and
  // the material's own diffuse reflectance removes it from the diffuse term.
  // Receivers no probe covers yet (a disocclusion, a viewmodel) keep the ambient term.
  vec4 incoming = cascadeDiffuse(position, normal);
  lighting.diffuse = mix(fallback, incoming.rgb, incoming.a);
  lighting.reflectionReference = incoming.a > 0;
  return lighting;
}

// Live incident lighting: no frozen capture shares this domain, so cubemaps
// stay unnormalized, while the sky fallback is limited by it.
uint diffuseReflectionReferenceDomain() { return LiveIncidentLightingReference; }
