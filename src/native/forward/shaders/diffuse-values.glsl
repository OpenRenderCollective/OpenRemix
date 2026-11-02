#ifndef OPENREMIX_DIFFUSE_VALUES
#define OPENREMIX_DIFFUSE_VALUES
// Analytic lobes requested in addition to the incident field. Attribution
// preserves Source 2 per-light residuals; it is independent of lobe ownership.
struct DirectLightingPolicy {
  bool diffuse;
  bool specular;
  bool attribution;
};
DirectLightingPolicy analyticDirectLighting(bool attribution) {
  return DirectLightingPolicy(true, true, attribution);
}
// Incident diffuse irradiance / pi, before material response or exposure.
struct ReceiverLighting {
  vec3 diffuse;
  // A source owning all direct diffuse can disable that analytic lobe without
  // pretending its energy belongs to the baked per-light attribution system.
  DirectLightingPolicy direct;
  // This field can supply the local incident-light reference used by the
  // selected reflection provider. Independent of direct-light membership.
  bool reflectionReference;
};
vec3 composeDiffuseLight(ReceiverLighting lighting, vec3 diffuseReflectance) {
  return diffuseReflectance * lighting.diffuse;
}
#endif
